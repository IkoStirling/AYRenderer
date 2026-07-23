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
//
// §P5.5 D (2026-07-23) — bump v0 → v1. Cache-key bump
// `v0_equirect_fullscreen` → `v1_equirect_or_cube_perpixel_dir`.
// Phoskia source now declares BOTH `texture2d skyEquirect` AND
// `texturecube skyCube` + `uniform float skyKind` (0 = Equirect,
// 1 = CubeMap) + `uniform vec4 skyMix`. The FS picks one of two
// per-pixel paths via `mix(equirectColor, cubeColor, skyKind)`.
//
// §Skybox-cam (2026-07-23) — bump v2 → v3. Root cause of "skybox
// doesn't follow freecam": the FS sampled equirect at screen UV and
// built cube dirs as fixed `normalize(ndc.x, -ndc.y, -1)` in a
// camera-locked view space. `setViewTransform(view, frame.view,
// frame.proj)` was already called, but the fullscreen VS emits
// clip-space positions directly so view/proj never affected the
// look direction. Fix: unproject NDC through
// `inverseProjectionMatrix`, rotate with `inverseViewMatrix`
// (w=0 ⇒ translation stripped), then sample equirect/cube from
// that world direction. Freecam → setCameraLookAt →
// setMainCameraLookAtPerspective → frame.view is live; Skybox now
// consumes it via bgfx builtins.
static constexpr const char* kSkyboxCacheKey =
    "skybox_v3_cam_invview_dir";

// §P5.5 D (2026-07-23) — Bug fix #3 (mirror
// LightingPass.cpp:69-72). Externalize the cache-key string for
// unit-test live-drift detection. Extern declared in
// SkyboxPass.h.
const char* const kSkyboxCacheKeyCStr = kSkyboxCacheKey;

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

// §Skybox0 (2026-07-23) + §P5.5 D (2026-07-23) — Phoskia Skybox
// VS/FS source.
//
// Sampler inputs:
//   - skyEquirect : color (RGB = panorama color, A unused — host
//                    can populate with any 2:1 panoramic texture
//                    via Renderer::createTextureFromRgba8 +
//                    SkySource::equirect)
//   - skyCube     : samplerCube (RGBA8 cube map — host uploads
//                    via Renderer::setSkySourceCube(TextureHandle)).
//                    Used only when skyKind=1 (mirror §P5.5 D
//                    hard rule: cube path active only when both
//                    SkySource::kind == CubeMap AND the cube
//                    handle is valid).
//
// Uniform inputs:
//   - skyMix      : vec4 — .x = intensity scalar (1.0 default;
//                            host can override per-material via
//                            Renderer::setMaterialVec3(material,
//                            "skyMix", v) to dim the backdrop)
//   - skyKind     : float (0.0 = Equirect, 1.0 = CubeMap). Per-
//                   frame uploaded by SkyboxPass::execute based
//                   on `ctx.skySource->kind == CubeMap &&
//                   cubeHandleValid`. Default 0 ⇒ equirect
//                   path (A-ship byte-equivalent).
//
// Math (both kinds — §Skybox-cam 2026-07-23):
//   ndc     = vUv * 2 - 1                         (pre Y-flip clip)
//   viewH   = inverseProjectionMatrix * (ndc,1,1)
//   viewDir = normalize(viewH.xyz)
//   worldD  = normalize((inverseViewMatrix * (viewDir, 0)).xyz)
//   Equirect: lon=atan(x,z), lat=asin(y) → UV
//   CubeMap : sample(skyCube, worldD)
//
// Final color = mix(equirectColor, cubeColor, skyKind).
constexpr const char* kSkyboxPhoskiaSource = R"(
material Skybox {
    texture2d skyEquirect
    texturecube skyCube
    uniform vec4 skyMix
    uniform vec4 skyKind
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        let ndcXY = vec2(vUv.x * 2.0 - 1.0, vUv.y * 2.0 - 1.0)
        let viewH = inverseProjectionMatrix * vec4(ndcXY.x, ndcXY.y, 1.0, 1.0)
        let viewDir = normalize(viewH.xyz)
        let worldH = inverseViewMatrix * vec4(viewDir.x, viewDir.y, viewDir.z, 0.0)
        let worldDir = normalize(worldH.xyz)
        let lon = atan2(worldDir.x, worldDir.z)
        let lat = asin(clamp(worldDir.y, -1.0, 1.0))
        let equirectUv = vec2(lon * 0.15915494309 + 0.5, lat * 0.31830988618 + 0.5)
        let equirectColor = sample(skyEquirect, equirectUv).xyz * skyMix.x
        let cubeColor = sample(skyCube, worldDir).xyz * skyMix.x
        let skyColor = mix(equirectColor, cubeColor, skyKind.x)
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
    // §P5.5 D — cube binding IDs reset on destroy so the next
    // ensureProgram() re-resolves them after a cache-key bump
    // (mirror equirect binding reset above).
    _tSkyCube             = ayt::shader::InvalidBinding;
    _uSkyKind             = ayt::shader::InvalidBinding;
    // §P5.5 D — cube producer handle reset on destroy. The host
    // is responsible for re-uploading via Renderer::setSkySourceCube
    // after a pipeline rebuild (cutsheet §P5.5 D producer-state
    // lifecycle mirrors shadowMap producer state).
    _skyCubeTexture       = ayt::render::TextureHandle{};
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
        // §P5.5 D — cube bindings reset on cache-key bump so
        // the next resolve path re-acquires them after v1
        // forces a re-acquire.
        _tSkyCube             = ayt::shader::InvalidBinding;
        _uSkyKind             = ayt::shader::InvalidBinding;
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
    // §P5.5 D (2026-07-23) — cube sampler + per-frame skyKind
    // uniform binding IDs. Default InvalidBinding on acquire
    // failure; the FS's mix() with skyKind=0 will collapse to the
    // equirect branch even if skyCube binding never resolves.
    _tSkyCube     = _program.getTextureBinding("skyCube");
    _uSkyKind     = _program.getUniformBinding("skyKind");
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

    // §Skybox0 (2026-07-23) + §P5.5 D (2026-07-23) — sky-source gate.
    // Two independent reasons to early-return 0:
    //   1. No host pointer passed ⇒ default Forward host sees no
    //      sky (cutsheet §Skybox0 "default host = no sky").
    //   2. SkySource is inactive (kind mismatch OR no handle
    //      uploaded). SkySource::isActive() encapsulates the
    //      per-kind active contract:
    //      - Equirect: equirect handle valid (host populated SkySource)
    //      - CubeMap : cubeMap handle valid (host populated SkySource)
    //      Note: `hasCubeActive()` (this pass's own cube handle) is
    //      NOT consulted here — only the host-supplied SkySource
    //      intent matters for the gate. The cube-side handle is
    //      consulted below when deciding skyKind=0 vs skyKind=1.
    const ayt::render::SkySource* sky = ctx.skySource;
    if (sky == nullptr || !sky->isActive()) {
        return 0;
    }

    // §P5.5 D (2026-07-23) — skyKind predicate: cube handle valid
    // AND host wants CubeMap kind. Hard rule: cube valid ⇒ CubeMap
    // path wins; otherwise equirect path. The two paths are
    // mutually exclusive per frame — never "each draws half".
    const bool cubeActive = hasCubeActive(sky->kind);

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

    // §Skybox0 (2026-07-23) + §P5.5 D — bind the per-kind texture
    // sampler. The Phoskia source declares BOTH `texture2d
    // skyEquirect` AND `texturecube skyCube`; per-frame we bind
    // whichever kind is active (skyKind uniform selects the branch
    // via `mix`). Equirect lookup goes through `ctx.textures`
    // (canonical map populated by RenderResourceManager when the
    // host calls Renderer::createTextureFromRgba8 / loadTexture).
    // Cube lookup uses this pass's `_skyCubeTexture` handle directly
    // (mirror shadowMap lookup in tryBindShadowSampler — producer
    // owns the bgfx::TextureHandle, the helper just binds it).
    bgfx::TextureHandle equirectHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle cubeHandle{BGFX_INVALID_HANDLE};
    if (!cubeActive) {
        const auto texIt = ctx.textures.find(sky->equirect.id);
        if (texIt == ctx.textures.end()
            || !BGFXAdapter::isValid(texIt->second.handle)) {
            return 0;
        }
        equirectHandle = texIt->second.handle;
    } else {
        // §P5.5 D — cube path. The cube handle lives on this pass's
        // producer state (set by Renderer::setSkySourceCube →
        // setCubeTexture). We look up the underlying bgfx ::
        // TextureHandle via ctx.textures using the handle id (mirror
        // equirect lookup — the cube handle is a TextureHandle
        // resource, same lifetime contract).
        const auto cubeIt = ctx.textures.find(_skyCubeTexture.id);
        if (cubeIt == ctx.textures.end()
            || !BGFXAdapter::isValid(cubeIt->second.handle)) {
            return 0;
        }
        cubeHandle = cubeIt->second.handle;
    }

    if (!cubeActive
        && _tSkyEquirect != ayt::shader::InvalidBinding) {
        const uint8_t stage = _program.getTextureStage(_tSkyEquirect);
        _program.setTexture(stage, _tSkyEquirect,
                            toShaderTexture(equirectHandle));
    } else if (cubeActive
               && _tSkyCube != ayt::shader::InvalidBinding) {
        const uint8_t stage = _program.getTextureStage(_tSkyCube);
        _program.setTexture(stage, _tSkyCube,
                            toShaderTexture(cubeHandle));
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

    // §P5.5 D (2026-07-23) — upload `skyKind` uniform. 0.0 =
    // Equirect path, 1.0 = CubeMap path. The Phoskia FS uses
    // `mix(equirectColor, cubeColor, skyKind)` to pick one branch;
    // when skyKind=0 the cube branch's `sample(skyCube, ...)` reads
    // 0 (the FS still evaluates but `mix` discards) — same for
    // skyKind=1 vs the equirect branch. Default 0 ⇒ pre-D
    // byte-equivalent (host never called setSkySourceCube).
    //
    // Upload-shape note: bgfx's `setUniform` writes a vec4 slot
    // regardless of the Phoskia-declared type. AYShaderPool maps
    // `uniform float` → bgfx::UniformType::Vec4 (one Vec4 slot
    // = 16 bytes). All other pass uniforms (skyMix / bloom / ...)
    // use the same 16-byte padded upload pattern. Using
    // sizeof(float)=4 would under-write the slot — see the §P5.5 D
    // bug fix in LightingPass (cubeActive/ambientStrength pads).
    if (_uSkyKind != ayt::shader::InvalidBinding) {
        const float skyKindPad[4] = {
            cubeActive ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f
        };
        _program.setUniform(_uSkyKind, skyKindPad, sizeof(skyKindPad));
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
