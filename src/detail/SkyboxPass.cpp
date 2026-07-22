#include "detail/SkyboxPass.h"

#include "detail/BgfxMatrix.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/RenderPass.h"
#include "detail/SkySource.h"

#include <cstdio>

namespace ayt::render::detail
{

// §Skybox0 (2026-07-23) — build stamp literal (mirror
// LightingPass.cpp:21 `kLightingBuildStamp`). Pointer-equal compare;
// bumping this triggers a FBO rebuild on next execute(). Bumping
// is safe across future Skybox cuts.
static constexpr const char* kSkyboxBuildStamp = "sky0-2026-07-23";

// §Skybox0 (2026-07-23) — cache key literal (mirror
// LightingPass.cpp:70-71 `kLightingCacheKey`). Pointer-equal
// compare so cache invalidates when the literal bumps.
// `s_acquiredCacheKey` static guard inside ensureProgram() forces
// re-acquire.
//
// Phoskia source declares `material Skybox { texture2d skyEquirect;
// uniform vec4 skyMix; }` — the FS is the simplest possible
// equirect-panorama sampler: directly sample at the screen UV
// (equirect maps are 2:1 panoramic sphere projections, so
// screen UV == panorama UV without any lat/long → 3D dir
// conversion). Future IBL cut may need the dir conversion to do
// ambient cube convolution, but for pure backdrop sampling this
// is byte-equivalent and lets the FS stay cheap (1 sample +
// 1 multiply).
//
// MVP A scope: `texture2d skyEquirect` (kind=Equirect) only.
// CubeMap kind → SkyboxPass early-returns 0 (CubeMap sampler
// path is reserved for §Skybox0-B).
static constexpr const char* kSkyboxCacheKey =
    "skybox_v0_equirect_fullscreen";

// §Skybox0 (2026-07-23) — fullscreen-triangle vertex data,
// duplicated from LightingPass.cpp:85-89 (private state there —
// coupling would require a friend class, duplicate is cheaper).
// Same 3-vert NDC oversize-triangle bgfx pattern; same
// FullscreenVertex {x,y,u,v} layout that vertexLayoutPosUv()
// emits.
struct alignas(16) SkyboxFullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr SkyboxFullscreenVertex kSkyboxFullscreenTriangle[3] = {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    {  3.0f, -1.0f, 2.0f, 1.0f },
    { -1.0f,  3.0f, 0.0f, -1.0f },
};

constexpr uint16_t kSkyboxFullscreenIndices[3] = { 0, 1, 2 };

// §Skybox0 (2026-07-23) — Phoskia Skybox VS/FS source.
//
// Sampler inputs:
//   - skyEquirect : color (RGB = panorama color, A unused — host
//                    can populate with any 2:1 panoramic texture
//                    via Renderer::createTextureFromRgba8 +
//                    SkySource::equirect)
//
// Uniform inputs:
//   - skyMix      : vec4 — .x = intensity scalar (1.0 default;
//                            host can override per-material via
//                            Renderer::setMaterialVec3(material,
//                            "skyMix", v) to dim the backdrop)
//
// Math (simple equirect panorama blit):
//   uv = vec2(vUv.x, 1.0 - vUv.y)   // Y-flip for RT vs backbuffer
//   skyColor = sample(skyEquirect, uv).xyz * skyMix.x
//   return vec4(skyColor, 1.0)
//
// The `let` chain uses the same surface as the B4b GBufferFill /
// B5 Lighting source (verified at PR-F2/PR-F3/B5 ship) — `let`
// declarations + arithmetic + sample() texture lookups. No MRT,
// no bone palettes, no shadow compare — falls back to the
// legacy `return → gl_FragColor` path.
constexpr const char* kSkyboxPhoskiaSource = R"(
material Skybox {
    texture2d skyEquirect
    uniform vec4 skyMix
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let baseUv = vec2(vUv.x, 1.0 - vUv.y)
        let skyColor = sample(skyEquirect, baseUv).xyz * skyMix.x
        return vec4(skyColor, 1.0)
    }
}
)";

SkyboxPass::~SkyboxPass() = default;

void SkyboxPass::setOutputSize(uint16_t width, uint16_t height) noexcept
{
    // §Skybox0 (2026-07-23) — host-driven store-only call (mirror
    // LightingPass::setOutputSize at LightingPass.cpp:253-260).
    // No adapter access here; the next execute() honors the size.
    _skyW = width;
    _skyH = height;
}

void SkyboxPass::destroyResources(BGFXAdapter& adapter)
{
    // §Skybox0 (2026-07-23) — mirror LightingPass::destroyResources
    // at LightingPass.cpp:262-292 + GBufferPass::destroyResources
    // at GBufferPass.cpp:355-383. Drop the FBO, fullscreen VB/IB,
    // and reset all cached handles. W/H + buildStamp reset
    // UNCONDITIONALLY so a host that calls
    // setOutputSize(800,600) → destroyResources() expects W/H back
    // to 0. The FBO/VB/IB handles are only destroyed when actually
    // allocated (calling bgfx::destroy on an invalid handle is a
    // UAF on some bgfx backends — mirror ShadowMapResources::
    // destroy guard).
    if (BGFXAdapter::isValid(_skyFbo)) {
        adapter.destroy(_skyFbo);
        _skyFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = bgfx::VertexBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = bgfx::IndexBufferHandle{BGFX_INVALID_HANDLE};
    }
    _skyRt       = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _skyW        = 0;
    _skyH        = 0;
    _allocatedW  = 0;
    _allocatedH  = 0;
    _buildStamp  = "";
    _program.reset();
    _programReady         = false;
    _programAcquireFailed = false;
    _tSkyEquirect         = ayt::shader::InvalidBinding;
    _uSkyMix              = ayt::shader::InvalidBinding;
}

void SkyboxPass::ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    // §Skybox0 (2026-07-23) — mirror LightingPass::ensure at
    // LightingPass.cpp:294-337. Stamp-changed fast path + same-size
    // fast path; rebuild path on size/stamp change.
    if (!adapter.isInitialized() || width == 0 || height == 0) {
        return;
    }

    const bool stampChanged = (_buildStamp != kSkyboxBuildStamp);
    if (stampChanged) {
        _buildStamp = kSkyboxBuildStamp;
    }

    if (bgfx::isValid(_skyFbo)
        && _allocatedW == width
        && _allocatedH == height
        && !stampChanged) {
        return;  // FBO cached
    }

    // Rebuild path — drop old FBO + recreate
    if (bgfx::isValid(_skyFbo)) {
        adapter.destroy(_skyFbo);
        _skyFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _skyRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _allocatedW = _allocatedH = 0;
    }

    // §Skybox0 (2026-07-23) — 1× RGBA8 SkyOutput FBO. NO depth
    // attachment — sky is at infinity (cutsheet §Skybox0 "sky at
    // infinity"). `withDepth=false` matches LightingPass's
    // LightingOutput FBO pattern (LightingPass.cpp:328-330).
    _skyFbo = adapter.createFrameBuffer(width, height,
                                        bgfx::TextureFormat::RGBA8,
                                        /*withDepth=*/false);
    if (bgfx::isValid(_skyFbo)) {
        _allocatedW = width;
        _allocatedH = height;
        _skyW = width;
        _skyH = height;
        cacheAttachments(adapter);
    }
}

void SkyboxPass::cacheAttachments(BGFXAdapter& adapter)
{
    // §Skybox0 (2026-07-23) — mirror GBufferPass::cacheAttachments
    // at GBufferPass.cpp:435-452. The RT0 attachment is owned by
    // `_skyFbo` (destroyTextures=true upstream), so we read it
    // via `adapter.getFboAttachment` and never call bgfx::destroy
    // on it.
    _skyRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    if (!bgfx::isValid(_skyFbo)) {
        return;
    }
    _skyRt = adapter.getFboAttachment(_skyFbo, 0);
}

void SkyboxPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    // §Skybox0 (2026-07-23) — mirror PostProcessPass
    // ::ensureFullscreenQuad at PostProcessPass.cpp:293-322 +
    // LightingPass::ensureFullscreenQuad at LightingPass.cpp
    // :339-356. Lazy creation; VB/IB cached after first call.
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    _fullscreenVB = adapter.createVertexBuffer(kSkyboxFullscreenTriangle,
                                               sizeof(kSkyboxFullscreenTriangle),
                                               layout,
                                               BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kSkyboxFullscreenIndices,
                                              sizeof(kSkyboxFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void SkyboxPass::ensureProgram(ayt::shader::ShaderResourcePool& pool)
{
    // §Skybox0 (2026-07-23) — mirror LightingPass::ensureProgram
    // at LightingPass.cpp:358-397. Stamp-checked
    // `s_acquiredCacheKey` pointer-equal guard (ShadowCaster Issue
    // 1 fix at ShadowCaster.cpp:60-65). On compile failure: log +
    // set _programAcquireFailed, leave _programReady false. On
    // success: _program = acquired, _programReady = true.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kSkyboxCacheKey) {
        _program.reset();
        _programReady         = false;
        _programAcquireFailed = false;
        _tSkyEquirect         = ayt::shader::InvalidBinding;
        _uSkyMix              = ayt::shader::InvalidBinding;
        s_acquiredCacheKey    = kSkyboxCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }

    ayt::shader::ShaderResource acquired =
        pool.acquire(kSkyboxPhoskiaSource, kSkyboxCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[SkyboxPass] acquire failed; SkyboxPass will "
                     "run as no-op (no GPU draw). Errors:\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[SkyboxPass]   %s\n", err.c_str());
        }
        return;
    }

    std::fprintf(stderr,
                 "[SkyboxPass] program ready via Phoskia (cacheKey=%s)\n",
                 kSkyboxCacheKey);

    _program      = acquired;
    _programReady = true;

    // §Skybox0 (2026-07-23) — lazy-resolve binding IDs after
    // successful acquire. First execute() uses these; subsequent
    // frames reuse the cached IDs.
    _tSkyEquirect = _program.getTextureBinding("skyEquirect");
    _uSkyMix      = _program.getUniformBinding("skyMix");
}

uint32_t SkyboxPass::execute(PassExecContext& ctx)
{
    // §Skybox0 (2026-07-23) — first GPU work in the SkyboxPass.
    // Phoskia VS/FS acquires, binds view 6 to the SkyOutput FBO,
    // dispatches a fullscreen triangle that samples the host's
    // equirect texture and writes the panorama backdrop.
    //
    // Failure modes (cutsheet §1.7 "no FBO/work" signal pattern):
    //   - adapter not initialized             → return 0
    //   - Noop backend                       → return 0
    //   - Output size not yet set             → return 0
    //   - ctx.skySource == nullptr           → return 0
    //   - skySource->kind != Equirect        → return 0 (CubeMap
    //                                          reserved for B cut)
    //   - skySource->equirect invalid handle → return 0
    //   - Texture handle not in texture map  → return 0
    //   - FBO / VB / IB / program not ready  → return 0
    //
    // All paths return 0 draws — RenderPipeline's executeAll
    // accumulates 0 from this slot. Default Forward pipeline
    // doesn't include this slot at all (cutsheet §5.3 red line
    // #4 — Forward host 0 behavior change).
    if (!ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()) {
        return 0;
    }

    // Disable signal: host called setOutputSize(0, 0) (or never
    // called it). Mirror LightingPass.cpp:438-440 size==0
    // early-out.
    if (_skyW == 0 || _skyH == 0) {
        return 0;
    }

    // §Skybox0 (2026-07-23) — sky-source gate. Three independent
    // reasons to early-return:
    //   1. No host pointer passed ⇒ default Forward host sees no
    //      sky (cutsheet §Skybox0 "default host = no sky").
    //   2. Kind != Equirect ⇒ CubeMap path reserved for B cut; A
    //      ships equirect only. Setting CubeMap today is a safe
    //      no-op (no crash, just no sky on screen).
    //   3. equirect handle invalid (TextureHandle{} default) ⇒
    //      host populated the kind but forgot to allocate a
    //      texture. Same safe no-op semantics.
    const ayt::render::SkySource* sky = ctx.skySource;
    if (sky == nullptr
        || sky->kind != ayt::render::SkySourceKind::Equirect
        || !sky->hasEquirect()) {
        return 0;
    }

    ensure(ctx.adapter, _skyW, _skyH);
    if (!bgfx::isValid(_skyFbo)) {
        return 0;
    }

    ensureFullscreenQuad(ctx.adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensureProgram(ctx.pool);
    if (!_program.isValid()) {
        return 0;
    }

    // §Skybox0 (2026-07-23) — bind the equirect texture the host
    // referenced in SkySource::equirect. Look up the actual
    // bgfx::TextureHandle via ctx.textures (the canonical map
    // populated by RenderResourceManager when the host calls
    // Renderer::createTextureFromRgba8 / loadTexture).
    const auto texIt = ctx.textures.find(sky->equirect.id);
    if (texIt == ctx.textures.end()
        || !BGFXAdapter::isValid(texIt->second.handle)) {
        return 0;
    }

    if (_tSkyEquirect != ayt::shader::InvalidBinding) {
        const uint8_t stage = _program.getTextureStage(_tSkyEquirect);
        _program.setTexture(stage, _tSkyEquirect,
                            toShaderTexture(texIt->second.handle));
    }

    // §Skybox0 (2026-07-23) — upload `skyMix` uniform. Default
    // = 1.0 (full intensity). Host can override per-material via
    // `Renderer::setMaterialVec3(material, "skyMix", v)`. Phoskia
    // Vec4 ABI (bgfx Vec4 slot, see docs/pass-lessons-from-shadow.md
    // §3.1) — scalar in .x, pad .yzw = 0.
    if (_uSkyMix != ayt::shader::InvalidBinding) {
        const float skyMixPad[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        _program.setUniform(_uSkyMix, skyMixPad, sizeof(skyMixPad));
    }

    // §Skybox0 (2026-07-23) — view 6 wiring: bind SkyOutput FBO,
    // set rect to viewport size (full, no editor panel offset —
    // mirror LightingPass::execute view-8 wiring at
    // LightingPass.cpp:464-475), clear, dispatch fullscreen
    // triangle.
    const uint8_t viewId = kSkyboxViewId;
    ctx.adapter.setViewTransform(viewId, ctx.frame.view,
                                 ctx.frame.projection);
    ctx.adapter.setViewFrameBuffer(viewId, _skyFbo);
    ctx.adapter.setViewRect(viewId, 0, 0, _skyW, _skyH);
    // Clear to black (matches cutsheet §5.2 "GBuffer/Lighting 默认
    // clear=0" lock — black is the correct "no sky contribution"
    // baseline; lit fragments overwrite via the sky sampler).
    ctx.adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR,
                                /*rgba=*/0x000000ff,
                                /*depth=*/1.0f,
                                /*stencil=*/0);

    // §Skybox0 (2026-07-23) — sky state: WRITE_RGB | WRITE_A |
    // DEPTH_TEST_ALWAYS (no depth read, no depth write — sky is
    // at infinity, the depth attachment is absent). Same state
    // combination as LightingPass's post-process blit (verified
    // at LightingPass.cpp:488 `setStateDepthTestAlways()`).
    ctx.adapter.setStateDepthTestAlways();

    // §Skybox0 (2026-07-23) — fullscreen triangle dispatch.
    // SkyboxPass submits exactly 1 draw (the fullscreen triangle),
    // not N. Mirror LightingPass::execute at LightingPass.cpp
    // :780-787.
    ctx.adapter.setTransform(ayt::math::Float4x4::identity());
    ctx.adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    ctx.adapter.setIndexBuffer(_fullscreenIB, 0, 3);

    ayt::shader::DrawCallContext submitCtx;
    submitCtx.viewId = viewId;
    submitCtx.state  = 0;  // state owned by Adapter
    _program.submit(submitCtx);

    return 1;  // Skybox ships exactly 1 draw (fullscreen triangle)
}

} // namespace ayt::render::detail
