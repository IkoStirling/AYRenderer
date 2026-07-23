#pragma once

// §P5 B3 (2026-07-22) — LightingPass empty shell.
// §P5 B5 (2026-07-22) — LightingPass fullscreen-triangle Lambert
// directional light consumer.
//
// Mirrors GBufferPass's plumbing shape (PR-§P5 B2, 2026-07-22):
// a derived RenderPass that owns its producer state (the lighting
// output FBO + sampler bindings to the GBuffer MRT) and exposes
// them via non-owning accessors that downstream consumers (future
// B7+ multi-light consumers; B6 PostProcessPass will consume via
// `ctx.gbufferPass->lightingOutputFbo()` instead of `ctx.sceneFbo`
// per `pass-lessons-from-deferred.md:169` "B6 must change PP source
// priority") read through `PassExecContext::lightingPass`.
//
// B5 ships:
//   - LightingOutput FBO (lightingFbo, RGBA8, viewport size — 1×
//     color RT, NO depth attachment since this is a fullscreen
//     post-process pass that does not read depth).
//   - Fullscreen triangle VB/IB (mirror PostProcessPass shape, but
//     owned privately — duplicate the kFullscreenTriangle constant
//     rather than exposing PostProcessPass's private state).
//   - Phoskia `material Lighting` source (Lambert directional, 3
//     samplers: gbufferAlbedo/gbufferNormal/gbufferMotion + 3 vec4
//     uniforms: u_lightDirection / u_lightColor / u_cameraPos).
//   - `execute()` dispatches the fullscreen triangle on view 8
//     (cutsheet §5.1 lock + kLightingViewId=8 already shipped in B3)
//     bound to the LightingOutput FBO, with the 3 GBuffer samplers
//     bound via the borrowed pointer (`ctx.gbufferPass->gbufferAlbedoRt()
//     etc.`) and the 3 light uniforms uploaded from `ctx.frame`.
//
// B5 deliberately does NOT:
//   - Touch FrameContext (sizeof守门)
//   - Add RenderScene::Light struct (永久退休)
//   - Read shadowMap (B5.5 boundary — deferred to next cut)
//   - Use motion attachment as input (motion is for B7+ TAA, B5 only
//     samples albedo + normal)
//   - Change RenderPass::execute signature
//
// Cutsheet §5.3 red lines we still respect:
//   - No FrameContext writeback of lighting FBO
//   - No RenderScene::Light struct added
//   - No execute(PassExecContext&) signature change
//   - Public header (include/*.h) gets only the bool lightingEnabled()
//     getter — no bgfx type leaks (this file is in src/detail/, not
//     public surface)
//
// Lifetime: LightingPass instances are owned by RenderPipeline via
// unique_ptr. Borrowed pointer in PassExecContext stays valid for
// the duration of executeAll(). RenderPipeline outlives every
// render() call (passes owned via unique_ptr).

#include "detail/BGFXAdapter.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/SkyboxPass.h"

#include <AYShaderResource.h>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class LightingPass : public RenderPass {
public:
    // §P5 B5 (2026-07-22) — LightingPass fullscreen triangle on
    // view 8 (reserved per docs/pass-lessons-from-deferred.md §5.1).
    // B3 already shipped kLightingViewId = 8 — the constant is the
    // contract; B5 consumes it.
    static constexpr uint8_t  kLightingViewId      = 8;
    static constexpr uint16_t kLightingDefaultSize = 1280;

    LightingPass() = default;
    ~LightingPass() override;

    std::string_view name() const override { return "Lighting"; }

    uint32_t execute(PassExecContext& ctx) override;

    // §P5 B5 (2026-07-22) — isReady() flips to true once BOTH the
    // FBO ensure succeeded AND the Phoskia program is acquired.
    // B3 shipped `return false` unconditionally as a placeholder;
    // B5 wires the real expression. Mirror GBufferPass.cpp:92's
    // `idx != UINT16_MAX` FBO-ready check, AND-gated with
    // `_programReady` so consumers see a meaningful "can this pass
    // actually run on the current backend" signal (cutsheet §1.7
    // "no FBO/work" semantic — if either is missing, fall through
    // to early-return 0 in execute()).
    bool isReady() const noexcept {
        return _lightingFbo.idx != UINT16_MAX && _programReady;
    }

    // Stub accessors — return invalid handle / 0 until B5 wires
    // real GPU state. The output FBO is the consumer-side binding
    // (PostProcessPass, tone-mapping, future B7+ multi-light chain).
    bgfx::FrameBufferHandle lightingFbo()        const noexcept { return _lightingFbo; }
    bgfx::FrameBufferHandle lightingOutputFbo()  const noexcept { return _lightingFbo; }
    uint16_t                lightingWidth()      const noexcept { return _lightingW; }
    uint16_t                lightingHeight()     const noexcept { return _lightingH; }

    // B5 mirrors GBufferPass B4b's public plumbing shape: setOutputSize
    // stays as a host-driven store-only call (no adapter access),
    // and the next execute() honors the size. The actual FBO
    // creation is deferred to ensure() (called when adapter is
    // initialized). destroyResources() mirrors GBufferPass::destroy
    // (GBufferPass.cpp:240-266) — drops the FBO + fullscreen VB/IB
    // + program handle, resets all state.
    void setOutputSize(uint16_t width, uint16_t height) noexcept;
    void destroyResources(BGFXAdapter& adapter);

    // §P5 B5 (2026-07-22) — lazy Phoskia Lighting VS/FS acquire.
    // Mirrors GBufferPass::ensureProgram shape (GBufferPass.cpp:72-107)
    // and PostProcessPass::ensureProgram shape (PostProcessPass.cpp:311+).
    // Stamp-checked `static const char* s_acquiredCacheKey != kLightingCacheKey`
    // invalidates the cached program when the literal bumps.
    void ensureProgram(ayt::shader::ShaderResourcePool& pool);
    bool isProgramReady() const noexcept { return _programReady; }

    // §P5 B5 (2026-07-22) — build stamp pointer (mirror
    // GBufferPass::buildStamp() at GBufferPass.h:117). Pointer-equal
    // compare; default "" = never ensured.
    const char* buildStamp() const noexcept { return _buildStamp; }

private:
    // §P5 B5 (2026-07-22) — internal ensure path (mirror
    // GBufferPass::ensure at GBufferPass.cpp:268-314). Called from
    // execute() AFTER setOutputSize has stored the request. Creates
    // the 1× RGBA8 FBO via `adapter.createFrameBuffer(w, h, RGBA8,
    // withDepth=false)`, caches the result, and bumps buildStamp.
    void ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height);

    // §P5 B5 (2026-07-22) — internal lazy fullscreen triangle VB/IB
    // creation (mirror PostProcessPass::ensureFullscreenQuad at
    // PostProcessPass.cpp:280-309). We deliberately DUPLICATE the
    // `kFullscreenTriangle` constant rather than expose
    // PostProcessPass's private state — both passes happen to use
    // the same 3-vert NDC oversize triangle (bgfx fullscreen-quad
    // pattern, verified at PostProcessPass.cpp:15-33), and coupling
    // them would either need a friend class or a public helper.
    // Duplicate-constant is cheaper than the coupling.
    void ensureFullscreenQuad(BGFXAdapter& adapter);

    // §Skybox0 (2026-07-23) — gbufferSkyRt cached binding (mirror
    // gbufferAlbedoRt / gbufferNormalRt / gbufferMotionRt shape).
    // LightingPass FS samples the sky FBO that SkyboxPass produces
    // (via `ctx.skyboxPass->skyRt()`) to blend a backdrop into
    // unlit areas. Stored here for symmetry with the other GBuffer
    // RTs even though the actual handle comes from SkyboxPass — we
    // don't own the FBO, just cache the lookup result for tests.
    bgfx::TextureHandle     _gbufferSkyRt   = bgfx::TextureHandle{BGFX_INVALID_HANDLE};

    bgfx::FrameBufferHandle _lightingFbo     = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::VertexBufferHandle _fullscreenVB   = bgfx::VertexBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::IndexBufferHandle  _fullscreenIB   = bgfx::IndexBufferHandle{BGFX_INVALID_HANDLE};
    // Requested size (setOutputSize). ensure() caches against
    // _allocatedW/H — same deferred-resize bug as GBufferPass if
    // request and allocated share one pair of fields.
    uint16_t                _lightingW       = 0;
    uint16_t                _lightingH       = 0;
    uint16_t                _allocatedW      = 0;
    uint16_t                _allocatedH      = 0;
    const char*             _buildStamp      = "";

    // §P5 B5 (2026-07-22) — Phoskia Lighting VS/FS program (mirror
    // GBufferPass::_program at GBufferPass.h:157). Lazy-acquired by
    // ensureProgram(); consumed by execute() for the fullscreen
    // triangle submit.
    ayt::shader::ShaderResource _program;
    bool _programReady      = false;  // mirror GBufferPass _acquireFailed semantics: _program.isValid() OR _programReady=true (set on success); failure path leaves _programReady=false
    bool _programAcquireFailed = false;  // mirror GBufferPass _acquireFailed
};

// §P5.5 B (2026-07-23) — Bug fix #3: externalize the cache-key
// literal so unit tests can include this header and compare their
// mirror string against the live literal. Pre-B, kLightingCacheKey
// was a `.cpp` static (not addressable from outside), so tests
// fell back to string self-comparison ("mine == mine") and the
// drift detection was a no-op (false green — Test_B5 had been
// drifting silently since B7). The extern declaration here gives
// every test a single source of truth; drift = test fails immediately.
//
// Naming: `kLightingCacheKeyCStr` (CStr suffix = "raw C-string"
// per the AY naming rules). The actual string literal lives in
// LightingPass.cpp as the canonical definition.
extern const char* const kLightingCacheKeyCStr;

} // namespace ayt::render::detail