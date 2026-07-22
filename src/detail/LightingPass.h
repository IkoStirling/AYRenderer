#pragma once

// §P5 B3 (2026-07-22) — LightingPass empty shell.
//
// Mirrors GBufferPass's plumbing shape (PR-§P5 B2, 2026-07-22):
// a derived RenderPass that owns its producer state (the lighting
// output FBO + sampler bindings to the GBuffer MRT) and exposes
// them via non-owning accessors that downstream consumers (future
// B7+ multi-light consumers; the eventual tone-mapping pass in a
// later lane) read through `PassExecContext::lightingPass`.
//
// This B3 commit ships the SHELL ONLY:
//   - Class skeleton + RenderPass base contract (name + execute)
//   - isReady() = false always (no FBO, no program, no allocator)
//   - execute() Noop-gates on adapter state and returns 0 draws
//   - Stub accessors return BGFX_INVALID_HANDLE / 0 / 0
//   - destroyResources() is a clean no-op
//
// Real GPU work lands in B5:
//   - Lighting FBO (lightingOutputFbo, RGBA8, viewport size)
//   - Fullscreen triangle VS using vertexLayoutPosUv (P6.5 ship)
//   - FS that samples ctx.gbufferPass->gbufferAlbedoRt() /
//     gbufferNormalRt() / gbufferMotionRt() + ctx.shadowPass->shadowFbo()
//   - 1 directional light from FrameContext::lightDirection /
//     lightColor (B5 reuses existing primitive — NO new Light struct
//     per §5.3)
//
// §5.3 red lines we still respect:
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

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class LightingPass : public RenderPass {
public:
    // §P5 B5 (target) — LightingPass fullscreen triangle on view 8
    // (reserved per docs/pass-lessons-from-deferred.md §5.1). Until
    // then we use the bgfx-default invalid handle so anyone who
    // calls `lightingFbo()` gets a safe "no Lighting output this
    // frame" signal (same shape as GBufferPass::gbufferFbo() on Noop).
    static constexpr uint8_t  kLightingViewId      = 8;
    static constexpr uint16_t kLightingDefaultSize = 1280;

    LightingPass() = default;
    ~LightingPass() override;

    std::string_view name() const override { return "Lighting"; }

    uint32_t execute(PassExecContext& ctx) override;

    // B3: shell returns false unconditionally. B5 wires real FBO
    // + program + ensure() then flips to:
    //     return _lightingFbo.idx != UINT16_MAX && _programReady;
    bool isReady() const noexcept { return false; }

    // Stub accessors — return invalid handle / 0 until B5 wires
    // real GPU state. The output FBO is the consumer-side binding
    // (PostProcessPass, tone-mapping, future B7+ multi-light chain).
    bgfx::FrameBufferHandle lightingFbo()        const noexcept { return _lightingFbo; }
    bgfx::FrameBufferHandle lightingOutputFbo()  const noexcept { return _lightingFbo; }
    uint16_t                lightingWidth()      const noexcept { return _lightingW; }
    uint16_t                lightingHeight()     const noexcept { return _lightingH; }

    // B5 will move these into an internal `ensure()` like ShadowPass
    // does for `_mapResources.ensure()`. B3 leaves them as public
    // stubs so external resize code can be wired without ABI churn
    // when B5 lands.
    void setOutputSize(uint16_t width, uint16_t height) noexcept;
    void destroyResources(BGFXAdapter& adapter);

private:
    bgfx::FrameBufferHandle _lightingFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    uint16_t                _lightingW   = 0;
    uint16_t                _lightingH   = 0;
};

} // namespace ayt::render::detail