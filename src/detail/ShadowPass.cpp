#include "detail/ShadowPass.h"

#include "AYRenderer/ShadowConfig.h"
#include "detail/ShadowDebug.h"
#include "detail/ShadowDiagnostics.h"
#include "detail/ShadowMatrixBuilder.h"

#include <cmath>
#include <cstdio>

namespace ayt::render::detail
{

ShadowPass::~ShadowPass() = default;

uint32_t ShadowPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    ayt::shader::ShaderResourcePool& pool = ctx.pool;
    const uint8_t viewId = kShadowViewId;
    const auto& meshes    = ctx.meshes;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized() || adapter.isNoopBackend()) {
        return 0;
    }

    // P6.5 (2026-07-22) — bgfx::getCaps() replaced with Adapter
    // capability wrappers (capsTextureBlit / capsTextureReadBack).
    // logShadowCapsIfNeeded still takes the rendererType as a uint32
    // for stderr diagnostics; Adapter doesn't expose caps->rendererType
    // today (only the two capability flags we need), so we keep the
    // bgfx::getCaps() call here JUST for the rendererType number.
    // bgfx::Caps::rendererType is not a bgfx:: handle — it's a
    // bgfx::RendererType::Enum, used only for log; removing the
    // include entirely would require a second Adapter getter for
    // a single log line. Defer that to a future cleanup.
    const bgfx::Caps* caps = bgfx::getCaps();
    logShadowCapsIfNeeded(
        caps != nullptr ? static_cast<uint32_t>(caps->rendererType) : 999u,
        adapter.capsTextureBlit(),
        adapter.capsTextureReadBack());

    ++_frameCounter;

    // §P5.5 C (2026-07-23) — per-light shadow dispatch. Pick the
    // active caster set from the wired SceneLights (cutsheet
    // reservation: ctx.perLightShadows is the consumer view of
    // the same SceneLights instance). When null OR no light has
    // castShadow=true ⇒ pre-C byte-equivalent single-key-light
    // path: pick slot 0, build directional matrices from
    // FrameContext::lightDirection, populate lights[0] only.
    //
    // §P5.5 C size guard: Point light with castShadow=true is
    // OUT OF SCOPE (omni-shadow needs 6-face cubemap or
    // dual-paraboloid). Log + skip — FS contribution still gets
    // shadowKey=1.0 (no shadow).
    const ayt::render::SceneLights* lightsPtr = _sceneLightsRef;
    uint32_t activeCount = 0;
    // Initialize all slots to identity + zero-bias as the no-op
    // baseline; populate the active slots below.
    for (uint32_t i = 0; i < kShadowAtlasMaxSlots; ++i) {
        _atlasLightViewProjs[i] = ayt::math::Float4x4::identity();
        for (uint32_t c = 0; c < 16; ++c) {
            _atlasLightViewProjsCol[i][c] = (c % 5 == 0) ? 1.0f : 0.0f;
        }
        _atlasShadowBiases[i] = 0.0f;
    }

    // Decide effective requested size — atlas vs single-slot.
    // count==0 path keeps the historical single-slot size
    // (kDefaultShadowMapSize = 2048). count>0 path uses the
    // atlas size (kDefaultAtlasSize = 4096).
    if (_requestedSize == 0) {
        _requestedSize = kDefaultShadowMapSize;
    }
    const bool useAtlas =
        lightsPtr != nullptr
        && lightsPtr->count > 0
        && [lightsPtr]() -> bool {
            for (uint32_t i = 0; i < lightsPtr->count; ++i) {
                if (lightsPtr->lights[i].castShadow) return true;
            }
            return false;
        }();

    const uint16_t effectiveSize = useAtlas ? kDefaultAtlasSize
                                            : _requestedSize;

    if (useAtlas && _atlasLayout.slotCount == 0) {
        // Recompute layout if config changed since ctor (e.g. tests).
        _atlasLayout = computeShadowAtlasLayout(_atlasConfig);
    }

    _mapResources.ensure(adapter, effectiveSize, ayt::render::kShadowPipelineBuildStamp);
    if (!_mapResources.isValid()) {
        if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L1_Caps)) {
            std::fprintf(stderr,
                         "[ShadowPass] shadow FBO create failed at %ux%u; "
                         "shadow disabled for this frame\n",
                         effectiveSize, effectiveSize);
        }
        return 0;
    }

    _shadowCaster.ensureProgram(pool);
    const bool casterReady = _shadowCaster.isProgramReady();

    const bool homogeneousDepth = adapter.capsHomogeneousDepth();

    // Build the per-slot LVP matrices. Order matters: the active
    // castShadow slots are packed into the first `activeCount`
    // atlas sub-rects; the remaining slots stay identity (no-op).
    // Directional + Spot: buildDirectionalShadowMatricesForScene
    // builds an ortho light-space VP fitting the scene bounds —
    // correct for Directional; for Spot it's an approximation
    // (Spot's cone-frustum projection is left for a future cut;
    // Spot shadow in §P5.5 C uses the same scene-fit ortho MVP
    // as Directional, which gives a slightly loose shadow but
    // produces correct cast/receive behavior at no extra cost).
    // Point castShadow=true ⇒ log + skip (omni-shadow out of scope).
    if (useAtlas) {
        activeCount = 0;
        for (uint32_t i = 0; i < lightsPtr->count
                               && activeCount < _atlasLayout.slotCount;
             ++i) {
            const ayt::render::Light& L = lightsPtr->lights[i];
            if (!L.castShadow) continue;
            if (L.type == ayt::render::LightType::Point) {
                // Point omni-shadow out of scope for §P5.5 C.
                if (ayt::render::ShadowDiagnostics::enabled(
                        ayt::render::ShadowLogLevel::L1_Caps)) {
                    static uint32_t s_omniSkipLog = 0;
                    if (s_omniSkipLog < 4) {
                        std::fprintf(stderr,
                                     "[ShadowPass] Point light slot %u "
                                     "castShadow=true skipped (omni-shadow "
                                     "out of scope for §P5.5 C)\n",
                                     i);
                        ++s_omniSkipLog;
                    }
                }
                continue;
            }
            // Directional + Spot both use the scene-fit ortho VP
            // built from light direction. For Directional, this
            // matches pre-C behavior; for Spot, the cone isn't
            // honored in caster projection (cone-frustum MVP is
            // a future cut).
            buildDirectionalShadowMatricesForScene(
                scene,
                meshes,
                L.direction,
                _atlasLightViewProjs[activeCount],
                _lightProj,        // tmp scratch — same matrix used per slot
                _lightViewProj,    // tmp scratch
                _atlasLightViewProjsCol[activeCount],
                _lightProjCol,
                _lightViewProjCol,
                homogeneousDepth);
            _atlasShadowBiases[activeCount] = L.shadowBias;
            ++activeCount;
        }
    } else {
        // Pre-C byte-equivalent path: single key light from
        // FrameContext::lightDirection. lights[0] slot only.
        buildDirectionalShadowMatricesForScene(
            scene,
            meshes,
            ctx.frame.lightDirection,
            _lightView,
            _lightProj,
            _lightViewProj,
            _lightViewCol,
            _lightProjCol,
            _lightViewProjCol,
            homogeneousDepth);
        _atlasLightViewProjs[0] = _lightView;
        for (uint32_t c = 0; c < 16; ++c) {
            _atlasLightViewProjsCol[0][c] = _lightViewProjCol[c];
        }
        // Pre-C bias is the global uniform; per-slot bias is 0
        // (FS uses global fallback when 0).
    }
    _perLightShadowCount = activeCount;

    if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L3_Probe)) {
        logShadowPassCpuDiag(ctx.frame.lightDirection, homogeneousDepth, _lightViewProjCol);
    }

    _mapResources.bindShadowView(adapter, viewId, effectiveSize);
    // §P5.5 C — scissor the view to each sub-rect and re-draw
    // casters per atlas slot. Pre-C path has one caster pass into
    // the full atlas-sized sub-rect[0] (no scissor — covers the
    // entire atlas, byte-equivalent to a 2048×2048 FBO when
    // effectiveSize==2048). stats.atlasSlots reports slot count,
    // NOT draw-call count (see §S2-3).
    if (useAtlas) {
        // Disable scissor for slot 0 (covers the entire first
        // sub-rect because atlas size = slot0 size when N=8
        // and atlas=4096; setViewScissor(0,0,slotW,slotH)
        // matches the sub-rect pixel extent anyway, so we use
        // it for symmetry / clarity).
        for (uint32_t slot = 0; slot < activeCount; ++slot) {
            const ShadowAtlasPixelRect r = slotPixelRect(slot);
            adapter.setViewScissor(viewId, r.x, r.y, r.w, r.h);
            adapter.setViewTransformColumnMajor(
                viewId,
                _atlasLightViewProjsCol[slot],   // view (col-major)
                _lightProjCol);                  // proj (col-major) — shared
            adapter.touch(viewId);
            const uint64_t casterState = ShadowMapResources::casterDrawState();
            adapter.setStateDepthOnlyWrite();
            (void)_shadowCaster.drawCasters(
                adapter, viewId, casterState, scene, meshes);
        }
        // Reset scissor for subsequent consumers (next pass).
        adapter.setViewScissor(viewId, 0, 0,
                               static_cast<uint16_t>(effectiveSize),
                               static_cast<uint16_t>(effectiveSize));
    } else {
        adapter.setViewTransformColumnMajor(viewId, _lightViewCol, _lightProjCol);
        // P6.5 (2026-07-22) — bgfx::touch(viewId) replaced with
        // BGFXAdapter::touch() wrapper. Behavior identical (bgfx::touch
        // with isInitialized()-guarded no-op semantics).
        adapter.touch(viewId);

        // P6.5 (2026-07-22) — depth-write caster state now goes through
        // BGFXAdapter::setStateDepthOnlyWrite() preset instead of the
        // inline BGFX_STATE_WRITE_Z|DEPTH_TEST_LESS bit combination.
        // ShadowMapResources::casterDrawState() is kept as the
        // canonical byte-equivalent getter for tests / external callers
        // — we use it here because drawCasters() takes the state bits
        // as an argument (matches the adapter-side setState bits 1:1).
        const uint64_t casterState = ShadowMapResources::casterDrawState();
        adapter.setStateDepthOnlyWrite();
        if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L1_Caps)) {
            static uint32_t s_casterStateLog = 0;
            if (s_casterStateLog < 2) {
                std::fprintf(stderr,
                             "[ShadowDbg] caster depthTest=LESS writeZ=1 "
                             "(RGBA8 ndc01 + D24S8 ordering)\n");
                ++s_casterStateLog;
            }
        }

        (void)_shadowCaster.drawCasters(
            adapter, viewId, casterState, scene, meshes);
    }

    _mapResources.resolveForSampling(adapter, kShadowResolveViewId);

    const bgfx::TextureHandle shadowColor = _mapResources.colorAttachment(adapter);
    const ShadowProjectSample probe =
        projectWorldThroughLvpColMajor(_lightViewProjCol,
                                       ayt::math::FVector3(0.0f, 0.0f, 0.0f));

    const float maxCoord = static_cast<float>(effectiveSize > 1 ? effectiveSize - 1 : 0);
    ayt::render::ShadowFrameStats stats{};
    stats.frameIndex  = _frameCounter;
    stats.atlasSlots = activeCount;  // §S2-3 — renamed from casterDraws:
                                       // count of atlas sub-rect slots used
                                       // (= lit-light count, up to 8).
                                       // NOT mesh draw-call count.
    stats.casterReady = casterReady;
    stats.fboValid    = _mapResources.isValid();
    stats.blitOk      = lastBlitOk();
    stats.sampleReady = hasSampleableShadow();
    stats.probeU      = probe.shadowU;
    stats.probeV      = probe.shadowV;
    stats.expectEnc   = probe.refDepth;
    stats.probePxX    = static_cast<uint16_t>(std::lround(
        std::fmax(0.0f, std::fmin(probe.shadowU * maxCoord, maxCoord))));
    stats.probePxY    = static_cast<uint16_t>(std::lround(
        std::fmax(0.0f, std::fmin(probe.shadowV * maxCoord, maxCoord))));
    stats.rtIdx       = shadowColor.idx;
    stats.resolveIdx  = _mapResources.sampleTexture().idx;
    logShadowFrameStatsIfNeeded(stats);

    return activeCount;
}

void ShadowPass::destroyResources(BGFXAdapter& adapter)
{
    _mapResources.destroy(adapter);
    _shadowCaster.destroy(adapter);
}

} // namespace ayt::render::detail
