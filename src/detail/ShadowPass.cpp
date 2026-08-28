#include "detail/ShadowPass.h"

#include "AYRenderer/ShadowConfig.h"
#include "detail/ShadowDebug.h"
#include "detail/ShadowDiagnostics.h"
#include "detail/ShadowMatrixBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
// §P4 H1 (2026-08-24) — <bgfx/bgfx.h> removed from this TU. The
// rendererType log was the last direct bgfx::* call here; it now
// goes through BGFXAdapter::capsRendererType(). The cutsheet red
// line "pass files only call bgfx::* via BGFXAdapter" is satisfied.

namespace ayt::render::detail
{

ShadowPass::~ShadowPass() = default;

uint32_t ShadowPass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;
    _perLightShadowCount = 0;
    _sampleMapSize = 0;

    BGFXAdapter& adapter = ctx.adapter;
    ayt::shader::ShaderResourcePool& pool = ctx.pool;
    const uint8_t viewId = kShadowViewId;
    const auto& meshes    = ctx.meshes;
    const auto& textures  = ctx.textures;
    const auto& materials = ctx.materials;
    const RenderScene& scene = ctx.scene;

    if (!adapter.isInitialized() || adapter.isNoopBackend()) {
        // §P4 M1 (2026-08-24) — rate-limited early-return diagnostic
        // (Part 3 L11 helper). Without this a long-running headless
        // capture (Test_Shadow* suites) drowns the console in
        // "execute() returned 0" noise every frame.
        rateLimitedEarlyReturn("ShadowPass", "adapter not initialized / noop");
        return 0;
    }

    // §P4 H1 (2026-08-24) — bgfx::getCaps() replaced with
    // BGFXAdapter::capsRendererType(). The cached value comes from
    // bgfx_life::liveType() recorded at init time (= Count when
    // not initialized). Eliminates the last direct bgfx::* call in
    // this TU (cutsheet red line).
    logShadowCapsIfNeeded(
        adapter.capsRendererType(),
        adapter.capsTextureBlit(),
        adapter.capsTextureReadBack());

    // Stable IDs 18..25 avoid collisions with the main pipeline's 0..17
    // range. View 1 clears the full atlas, then tile views render with their
    // own viewport before view 2 resolves it. Reorder them ahead of consumers
    // every frame because bgfx resets view order at frame boundaries.
    static constexpr bgfx::ViewId kViewOrder[] = {
        1, 18, 19, 20, 21, 22, 23, 24, 25,
        2, 0,
        3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17
    };
    static_assert(sizeof(kViewOrder) / sizeof(kViewOrder[0]) == 26);
    adapter.setViewOrder(0, 26, kViewOrder);

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
    // §P4 L9 (2026-08-24) — collapsed the per-frame re-init
    // loop into a comment. The members are already identity at
    // construction (see ShadowPass.h M9 comment + the
    // initializer list). Per-frame, we now reset only the
    // slots we plan to overwrite this frame (the active range
    // computed below); slots >= activeCount keep their
    // identity default. The consumer (LightingPass) reads
    // `perLightShadowCount()` to bound its loop, so stale
    // values in slots >= activeCount are never sampled.
    // Direct test access (T6) reads the explicit active range.
    // (Kept as comments so future readers understand why we
    // don't memset the whole array each frame.)
    const auto resetAtlasSlot = [this](uint32_t slot) noexcept {
        _atlasLightViews[slot] = ayt::math::Float4x4::identity();
        _atlasLightProjs[slot] = ayt::math::Float4x4::identity();
        _atlasLightViewProjs[slot] = ayt::math::Float4x4::identity();
        for (uint32_t c = 0; c < 16; ++c) {
            _atlasLightViewsCol[slot][c] =
                (c % 5 == 0) ? 1.0f : 0.0f;
            _atlasLightProjsCol[slot][c] =
                (c % 5 == 0) ? 1.0f : 0.0f;
            _atlasLightViewProjsCol[slot][c] =
                (c % 5 == 0) ? 1.0f : 0.0f;
        }
        _atlasShadowBiases[slot] = 0.0f;
    };

    // Decide effective requested size — atlas vs single-slot.
    // count==0 path keeps the historical single-slot size
    // (kDefaultShadowMapSize = 2048). count>0 path uses the
    // atlas size (kDefaultAtlasSize = 4096).
    if (_requestedSize == 0) {
        _requestedSize = kDefaultShadowMapSize;
    }
    const bool hasSceneLights = lightsPtr != nullptr && lightsPtr->count > 0;
    const ShadowLightOrder lightOrder = hasSceneLights
        ? computeShadowLightOrder(*lightsPtr)
        : ShadowLightOrder{};
    const bool useAtlas = hasSceneLights && lightOrder.shadowCasterCount > 0;

    // A populated modern light list with no supported caster must not fall
    // back to the legacy key-light shadow or expose last frame's atlas.
    if (hasSceneLights && !useAtlas) {
        for (uint32_t i = 0; i < lightOrder.lightCount; ++i) {
            const uint32_t sourceIndex = lightOrder.indices[i];
            const ayt::render::Light& light = lightsPtr->lights[sourceIndex];
            if (light.castShadow
                && light.type == ayt::render::LightType::Point
                && ayt::render::ShadowDiagnostics::enabled(
                    ayt::render::ShadowLogLevel::L1_Caps)) {
                static uint32_t s_omniOnlySkipLog = 0;
                if (s_omniOnlySkipLog
                    < ayt::render::ShadowDiagnostics::kVerboseLogLimit) {
                    std::fprintf(stderr,
                                 "[ShadowPass] Point light slot %u "
                                 "castShadow=true skipped (omni-shadow "
                                 "out of scope for §P5.5 C)\n",
                                 sourceIndex);
                    ++s_omniOnlySkipLog;
                }
            }
        }
        return 0;
    }

    const uint16_t effectiveSize = useAtlas ? kDefaultAtlasSize
                                            : _requestedSize;

    if (useAtlas && _atlasLayout.slotCount == 0) {
        // Recompute layout if config changed since ctor (e.g. tests).
        _atlasLayout = computeShadowAtlasLayout(_atlasConfig);
        if (_atlasLayout.slotCount == 0) {
            return 0;
        }
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
    uint32_t casterDrawCount = 0;
    if (useAtlas) {
        activeCount = std::min(lightOrder.shadowCasterCount,
                               _atlasLayout.slotCount);

        // Point lights are deliberately excluded from the packed caster range.
        // Keep the existing diagnostic for hosts that request unsupported omni
        // shadows, while the shared order puts those lights in the unshadowed
        // suffix consumed by LightingPass.
        for (uint32_t i = 0; i < lightOrder.lightCount; ++i) {
            const uint32_t sourceIndex = lightOrder.indices[i];
            const ayt::render::Light& L = lightsPtr->lights[sourceIndex];
            if (L.castShadow && L.type == ayt::render::LightType::Point) {
                // Point omni-shadow out of scope for §P5.5 C.
                if (ayt::render::ShadowDiagnostics::enabled(
                        ayt::render::ShadowLogLevel::L1_Caps)) {
                    static uint32_t s_omniSkipLog = 0;
                    // §P4 L8 (2026-08-24) — replace magic 4
                    // with ShadowDiagnostics::kVerboseLogLimit
                    // (= 8). The Point-light skip is a
                    // structural diagnostic, not a frame
                    // summary; kVerboseLogLimit matches the
                    // "first N, then silent" convention for
                    // structural messages.
                    if (s_omniSkipLog
                        < ayt::render::ShadowDiagnostics::kVerboseLogLimit) {
                        std::fprintf(stderr,
                                     "[ShadowPass] Point light slot %u "
                                     "castShadow=true skipped (omni-shadow "
                                     "out of scope for §P5.5 C)\n",
                                     sourceIndex);
                        ++s_omniSkipLog;
                    }
                }
            }
        }

        for (uint32_t slot = 0; slot < activeCount; ++slot) {
            const uint32_t sourceIndex = lightOrder.indices[slot];
            const ayt::render::Light& L = lightsPtr->lights[sourceIndex];
            // Directional + Spot both use the scene-fit ortho VP
            // built from light direction. For Directional, this
            // matches pre-C behavior; for Spot, the cone isn't
            // honored in caster projection (cone-frustum MVP is
            // a future cut).
            // §P4 L9 (2026-08-24) — reset the active slot
            // before overwriting (the L9 helper replaces the
            // old "reset all slots up front" pattern).
            resetAtlasSlot(slot);
            for (uint32_t c = 0; c < 4; ++c) {
                _shadowSampleRects[slot][c] = _atlasLayout.subRects[slot][c];
            }
            buildDirectionalShadowMatricesForScene(
                scene,
                meshes,
                L.type == ayt::render::LightType::Spot
                    ? L.spotDirection
                    : L.direction,
                _atlasLightViews[slot],
                _atlasLightProjs[slot],
                _atlasLightViewProjs[slot],
                _atlasLightViewsCol[slot],
                _atlasLightProjsCol[slot],
                _atlasLightViewProjsCol[slot]);
            _atlasShadowBiases[slot] = L.shadowBias;
        }
    } else {
        // Pre-C byte-equivalent path: single key light from
        // FrameContext::lightDirection. lights[0] slot only.
        // §P4 L9 (2026-08-24) — reset atlas slot 0 before the
        // pre-C byte-equivalent single-light path overwrites
        // it. Without this, a previous frame's per-light path
        // would leave stale LVPs in slots 1..7 — the
        // `perLightShadowCount()==0` gate in LightingPass
        // would correctly skip them, but direct test access
        // (T6) would see stale data.
        resetAtlasSlot(0);
        _shadowSampleRects[0][0] = 0.0f;
        _shadowSampleRects[0][1] = 0.0f;
        _shadowSampleRects[0][2] = 1.0f;
        _shadowSampleRects[0][3] = 1.0f;
        buildDirectionalShadowMatricesForScene(
            scene,
            meshes,
            ctx.frame.lightDirection,
            _lightView,
            _lightProj,
            _lightViewProj,
            _lightViewCol,
            _lightProjCol,
            _lightViewProjCol);
        _atlasLightViews[0] = _lightView;
        _atlasLightProjs[0] = _lightProj;
        _atlasLightViewProjs[0] = _lightViewProj;
        for (uint32_t c = 0; c < 16; ++c) {
            _atlasLightViewsCol[0][c] = _lightViewCol[c];
            _atlasLightProjsCol[0][c] = _lightProjCol[c];
            _atlasLightViewProjsCol[0][c] = _lightViewProjCol[c];
        }
        // Pre-C bias is the global uniform; per-slot bias is 0
        // (FS uses global fallback when 0).
        activeCount = 1;
    }
    _perLightShadowCount = activeCount;

    if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L3_Probe)) {
        logShadowPassCpuDiag(ctx.frame.lightDirection, homogeneousDepth, _lightViewProjCol);
    }

    // §P5.5 C — scissor the view to each sub-rect and re-draw
    // casters per atlas slot. Pre-C path has one caster pass into
    // the full atlas-sized sub-rect[0] (no scissor — covers the
    // entire atlas, byte-equivalent to a 2048×2048 FBO when
    // effectiveSize==2048). stats.atlasSlots reports slot count,
    // NOT draw-call count (see §S2-3).
    if (useAtlas) {
        // Clear the shared atlas once using the full-size legacy view. Each
        // slot then uses a distinct tile-sized viewport. Scissor alone is not
        // sufficient: with a full-atlas viewport, NDC would map across 4096²
        // and clipping to a tile would retain only part of the projection.
        _mapResources.bindShadowView(adapter, viewId, effectiveSize,
                                     /*clearAttachments=*/true);
        adapter.touch(viewId);
        for (uint32_t slot = 0; slot < activeCount; ++slot) {
            const uint8_t slotViewId = static_cast<uint8_t>(
                kShadowAtlasFirstViewId + slot);
            const ShadowAtlasPixelRect r = slotPixelRect(slot);
            _mapResources.bindShadowView(adapter, slotViewId, effectiveSize,
                                         /*clearAttachments=*/false);
            adapter.setViewRect(slotViewId, r.x, r.y, r.w, r.h);
            adapter.setViewScissor(slotViewId, r.x, r.y, r.w, r.h);
            adapter.setViewTransformColumnMajor(
                slotViewId,
                _atlasLightViewsCol[slot],
                _atlasLightProjsCol[slot]);
            adapter.touch(slotViewId);
            const uint64_t casterState = ShadowMapResources::casterDrawState();
            adapter.setStateDepthOnlyWrite();
            casterDrawCount += _shadowCaster.drawCasters(
                adapter, slotViewId, casterState,
                scene, meshes, textures, materials);
        }
    } else {
        _mapResources.bindShadowView(adapter, viewId, effectiveSize);
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
            // §P4 L1 (2026-08-24) — replace magic 2 with
            // ShadowDiagnostics::kFrameSummaryLimit. Matches the
            // rate-limited convention used across the shadow
            // subsystem (M3/M4/M5 use kProbeLogLimit).
            if (s_casterStateLog
                < ayt::render::ShadowDiagnostics::kFrameSummaryLimit) {
                std::fprintf(stderr,
                             "[ShadowDbg] caster depthTest=LESS writeZ=1 "
                             "(RGBA8 ndc01 + D24S8 ordering)\n");
                ++s_casterStateLog;
            }
        }

        casterDrawCount = _shadowCaster.drawCasters(
            adapter, viewId, casterState, scene, meshes, textures, materials);
    }

    // A touched shadow view clears the map even when there are no drawable
    // casters; white depth encoding then correctly means fully lit.
    _producedThisFrame = true;
    _sampleMapSize = effectiveSize;

    _mapResources.resolveForSampling(adapter, kShadowResolveViewId);

    const bgfx::TextureHandle shadowColor = _mapResources.colorAttachment(adapter);
    const float* const probeLvp = useAtlas
        ? _atlasLightViewProjsCol[0]
        : _lightViewProjCol;
    const ShadowProjectSample probe =
        projectWorldThroughLvpColMajor(probeLvp,
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

    return casterDrawCount;
}

void ShadowPass::destroyResources(BGFXAdapter& adapter)
{
    _producedThisFrame = false;
    _perLightShadowCount = 0;
    _sampleMapSize = 0;
    _mapResources.destroy(adapter);
    _shadowCaster.destroy(adapter);
}

} // namespace ayt::render::detail
