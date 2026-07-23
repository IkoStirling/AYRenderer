#pragma once

// §Skybox0 (2026-07-23) — SkyboxPass: fullscreen-triangle Skybox
// pass on view 6 (slot 1 in 7-slot Deferred pipeline; between
// Shadow and GBuffer). Writes sceneSkyFbo (RGBA8) so LightingPass
// can sample it as a backdrop in unlit areas via
// `texture2d gbufferSky`.
//
// Mirrors GBufferPass / LightingPass plumbing shape: a derived
// RenderPass that owns its producer state (the sky FBO + the
// fullscreen triangle VB/IB + the Phoskia program) and exposes
// them via non-owning accessors that downstream consumers
// (LightingPass, read through `PassExecContext::skyboxPass`)
// borrow.
//
// Cutsheet §5.3 / §5.5 red lines preserved:
//   - NO FrameContext field additions
//   - NO RenderScene::Light re-introduction
//   - NO RenderPass::execute signature change
//   - NO default Forward pipeline behavior change (Forward
//     `makeDefault()` does not include the Skybox slot — Skybox is
//     opt-in via `makeDeferred()` or a custom desc).
//   - NO public header (include/*.h) gains a `bgfx::` type
//     (the SkySource POD is bgfx-free; only `bgfx::*Handle`
//     fields live in this detail header).
//
// Lifetime: SkyboxPass instances are owned by RenderPipeline via
// unique_ptr (same as ShadowPass / GBufferPass / LightingPass).
// The borrowed pointer in PassExecContext stays valid for the
// duration of executeAll(). Across frames, the pointer remains
// valid because the pipeline outlives every render() call.

#include "detail/BGFXAdapter.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include <AYShaderResource.h>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class SkyboxPass : public RenderPass {
public:
    // §Skybox0 (2026-07-23) — view-id allocation per cutsheet
    // §5.1 lock table. Pre-§Skybox0 view ids 0..5 were:
    //   0 = backbuffer / clear (default)
    //   1 = ShadowPass caster
    //   2 = ShadowPass resolve
    //   3 = ForwardOpaque / Transparent
    //   4 = PostProcessPass (forward path)
    //   5 = UI
    //   6 = (reserved — now Skybox)
    //   7 = GBuffer MRT (already shipped)
    //   8 = LightingPass fullscreen (already shipped)
    //   10 = PostProcessPass deferred blit (already shipped)
    // Skybox claims the reserved 6. The cutsheet §10 view-id table
    // is updated to reflect this in the §Skybox0 doc block.
    static constexpr uint8_t  kSkyboxViewId         = 6;
    static constexpr uint16_t kSkyboxDefaultSize    = 1280;
    static constexpr uint8_t  kSkyboxAttachmentCount = 1; // RT0 sky color

    SkyboxPass() = default;
    ~SkyboxPass() override;

    std::string_view name() const override { return "Skybox"; }
    uint32_t execute(PassExecContext& ctx) override;

    // §Skybox0 (2026-07-23) — isReady() flips to true once BOTH the
    // FBO ensure succeeded AND the Phoskia program is acquired.
    // Mirror LightingPass::isReady() shape (LightingPass.h:92-94) —
    // FBO idx valid AND program-ready.
    bool isReady() const noexcept {
        return _skyFbo.idx != UINT16_MAX && _programReady;
    }

    // Stub accessors — return invalid handle / 0 until the first
    // ensure() + ensureProgram() wire real GPU state.
    bgfx::FrameBufferHandle skyFbo()   const noexcept { return _skyFbo; }
    bgfx::TextureHandle     skyRt()    const noexcept { return _skyRt; }
    uint16_t                skyWidth()  const noexcept { return _skyW; }
    uint16_t                skyHeight() const noexcept { return _skyH; }

    // §P5.5 D (2026-07-23) — host-uploaded cube map handle for IBL
    // MVP. Lives on the SkyboxPass producer state (mirror shadowFbo
    // / lightingFbo / gbufferAlbedoRt producer-state pattern — the
    // cube is a producer resource owned by this pass). Renderer::
    // setSkySourceCube(TextureHandle) forwards to setCubeTexture().
    // Default = TextureHandle{} (invalid) = cube path inactive =
    // pre-D byte-equivalent flat ambient + flat equirect backdrop.
    //
    // hasCubeActive() returns true iff BOTH:
    //   1. The host called Renderer::setSkySourceCube with a valid
    //      TextureHandle (i.e., _skyCubeTexture.isValid()).
    //   2. ctx.skySource->kind == CubeMap (set by the host in the
    //      SkySource POD).
    // SkyboxPass::execute + LightingPass::execute both consult this
    // single predicate so the two paths agree per frame (cutsheet
    // §P5.5 D hard rule: "cube valid ⇒ CubeMap path; otherwise
    // equirect, never each draws half").
    //
    // Note: the SkySource->kind check happens at execute() time
    // because we don't store a borrowed-ptr mirror here (the host
    // owns the SkySource; the borrowed ptr lives in PassExecContext).
    // The accessor below is pure: returns the handle + validity
    // state. The hasCubeActive() overload takes the SkySource kind
    // so callers can decide.
    void setCubeTexture(ayt::render::TextureHandle cube) noexcept {
        _skyCubeTexture = cube;
    }
    ayt::render::TextureHandle cubeTexture() const noexcept {
        return _skyCubeTexture;
    }
    bool hasCubeTexture() const noexcept {
        return _skyCubeTexture.isValid();
    }
    // Combined predicate: cube handle valid AND host wants CubeMap
    // kind. SkyboxPass::execute and LightingPass::execute both call
    // this with the same arg; the resulting bool drives both FS
    // branches (SkyboxPass skyKind upload + LightingPass cubeActive
    // upload) so they can never disagree per frame.
    bool hasCubeActive(ayt::render::SkySourceKind kind) const noexcept {
        return _skyCubeTexture.isValid()
            && kind == ayt::render::SkySourceKind::CubeMap;
    }

    // §Skybox0 (2026-07-23) — host-driven store-only call (mirror
    // LightingPass::setOutputSize at LightingPass.cpp:253-260 and
    // GBufferPass::setGbufferSize at GBufferPass.cpp:346-353). No
    // adapter access here; the next execute() honors the size.
    void setOutputSize(uint16_t width, uint16_t height) noexcept;

    // §Skybox0 (2026-07-23) — mirror LightingPass::destroyResources
    // at LightingPass.cpp:262-292. Drop the FBO, fullscreen VB/IB,
    // and reset all cached handles. W/H + buildStamp reset
    // unconditionally so a host that calls setOutputSize(800,600)
    // → destroyResources() expects W/H back to 0.
    void destroyResources(BGFXAdapter& adapter);

    // §Skybox0 (2026-07-23) — lazy Phoskia Skybox VS/FS acquire.
    // Mirrors LightingPass::ensureProgram at LightingPass.cpp:358-397
    // (stamp-checked `static const char* s_acquiredCacheKey != ...`
    // invalidates the cached program when the literal bumps). On
    // compile failure: log + set _programAcquireFailed, leave
    // _programReady false. On success: _program = acquired,
    // _programReady = true.
    void ensureProgram(ayt::shader::ShaderResourcePool& pool);
    bool isProgramReady() const noexcept { return _programReady; }

    // §Skybox0 (2026-07-23) — build stamp pointer (mirror
    // GBufferPass::buildStamp at GBufferPass.h:117). Pointer-equal
    // compare; default "" = never ensured.
    const char* buildStamp() const noexcept { return _buildStamp; }

private:
    // §Skybox0 (2026-07-23) — internal ensure path (mirror
    // LightingPass::ensure at LightingPass.cpp:294-337 + GBufferPass
    // ::ensure at GBufferPass.cpp:385-433). Called from execute()
    // AFTER setOutputSize has stored the request. Creates the 1×
    // RGBA8 FBO via `adapter.createFrameBuffer(w, h, RGBA8,
    // withDepth=false)` (sky is at infinity, no depth attachment),
    // caches the RT0 attachment, and bumps buildStamp.
    void ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height);

    // §Skybox0 (2026-07-23) — internal lazy fullscreen triangle
    // VB/IB creation (mirror LightingPass::ensureFullscreenQuad at
    // LightingPass.cpp:339-356). We deliberately DUPLICATE the
    // `kFullscreenTriangle` constant rather than expose
    // LightingPass/PostProcessPass's private state — both passes
    // happen to use the same 3-vert NDC oversize triangle (bgfx
    // fullscreen-quad pattern, verified at LightingPass.cpp:78-91),
    // and coupling them would either need a friend class or a
    // public helper. Duplicate-constant is cheaper than coupling.
    void ensureFullscreenQuad(BGFXAdapter& adapter);

    // §Skybox0 (2026-07-23) — cache RT0 attachment handle from
    // `_skyFbo` so consumers (LightingPass via
    // `ctx.skyboxPass->skyRt()`) don't need to call
    // `adapter.getFboAttachment` themselves. Mirror
    // GBufferPass::cacheAttachments at GBufferPass.cpp:435-452.
    void cacheAttachments(BGFXAdapter& adapter);

    bgfx::FrameBufferHandle _skyFbo        = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle     _skyRt         = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::VertexBufferHandle _fullscreenVB  = bgfx::VertexBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::IndexBufferHandle  _fullscreenIB  = bgfx::IndexBufferHandle{BGFX_INVALID_HANDLE};
    // Requested size (setOutputSize). ensure() caches against
    // _allocatedW/H — must NOT reuse the request fields for the
    // cache check or a resize that only updates the request will
    // skip FBO rebuild (deferred viewport tears / jagged edges).
    // Mirror GBufferPass.cpp:175-179.
    uint16_t                _skyW          = 0;
    uint16_t                _skyH          = 0;
    uint16_t                _allocatedW    = 0;
    uint16_t                _allocatedH    = 0;
    const char*             _buildStamp    = "";

    // §Skybox0 (2026-07-23) — Phoskia Skybox VS/FS program
    // (mirror LightingPass::_program at LightingPass.h:162 +
    // GBufferPass::_program at GBufferPass.h:190). Lazy-acquired by
    // ensureProgram(); consumed by execute() for the fullscreen
    // triangle submit.
    ayt::shader::ShaderResource _program;
    // Mirror LightingPass _programReady semantics: _program.isValid()
    // OR _programReady=true (set on success); failure path leaves
    // _programReady=false (acquire failed → skip).
    bool _programReady         = false;
    bool _programAcquireFailed = false;

    // §P5.5 D (2026-07-23) — host-uploaded cube map handle. Producer
    // state (mirror shadowFbo / lightingFbo / gbufferAlbedoRt). Set
    // by Renderer::setSkySourceCube → setCubeTexture forward. Read
    // by execute() + LightingPass::execute via `cubeTexture()` /
    // `hasCubeActive()` accessors. Default = invalid = cube path
    // inactive = pre-D byte-equivalent.
    ayt::render::TextureHandle _skyCubeTexture{};

    // §Skybox0 (2026-07-23) — lazy-resolved binding IDs (mirror
    // LightingPass.cpp:498-528 binding-cache pattern). First execute
    // resolves; subsequent frames reuse the cached IDs. Default =
    // InvalidBinding (forces lazy resolve on first execute).
    ayt::shader::BindingId _tSkyEquirect  = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uSkyMix       = ayt::shader::InvalidBinding;
    // §P5.5 D (2026-07-23) — cube sampler + per-frame skyKind
    // uniform binding IDs. The cube sampler is declared in the
    // Phoskia source alongside the equirect sampler; the FS uses
    // `mix(equirectColor, cubeColor, skyKind)` to select one path.
    // Default = InvalidBinding; lazy-resolved after the cache-key
    // bump to v1 forces a re-acquire.
    ayt::shader::BindingId _tSkyCube      = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uSkyKind      = ayt::shader::InvalidBinding;
};

// §P5.5 D (2026-07-23) — Bug fix #3 mirror (see LightingPass.h:177-189
// for the originating pattern in §P5.5 B). Externalize the
// SkyboxPass cache-key literal so unit tests can include this
// header and compare their mirror against the live literal. Pre-D,
// kSkyboxCacheKey was a `.cpp` static, so the only comparison was
// self-compare ("mine == mine") = false-green drift detection.
// The extern declaration gives every test a single source of
// truth; drift = test fails immediately.
//
// Naming: `kSkyboxCacheKeyCStr` (CStr suffix = "raw C-string" per
// the AY naming rules). The actual string literal lives in
// SkyboxPass.cpp as the canonical definition.
extern const char* const kSkyboxCacheKeyCStr;

} // namespace ayt::render::detail
