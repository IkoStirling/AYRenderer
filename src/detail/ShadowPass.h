#pragma once

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace ayt::render::detail
{

// R5+ (Phase Shadow cut 1, 2026-07-20) — shadow caster pass slot per
// design.md §8.3. Compile-clean code; NOT addPass'd to the default
// pipeline. See `docs/execution-plan.md` §5 (Segfault Constraints) and
// §1.2 (subsystem status) before flipping on. Cut 2 (real light-space
// depth + opt-in enable) is gated by §5.4 isolation experiments.
//
// Today the pass exists so:
//   1) Hosts can opt into shadow casting by addPass()ing a
//      ShadowPass in front of their forward pipeline.
//   2) Tests can pin that the slot wires through RenderPipeline
//      without disturbing the existing 4-pass default.
//   3) Future work (cascaded shadow, Point/Spot shadow caster
//      programs, scene-AABB fit) has a well-typed home.
//
// v1 behavior (cut 1):
//   - Renders every DrawItem in scene.items() into a depth-only
//     FBO at the host-supplied shadow map size (default 1024).
//   - Uses bgfx's no-fragment-program submit (state = WRITE_Z |
//     DEPTH_TEST_LESS + DISCARD_ALL flag) to write depth without
//     fragment shader cost. Same trick every vanilla shadow-map
//     example uses.
//   - Light-space transform is **identity** (depth pre-pass equivalent);
//     the real orthographic light VP arrives in cut 2.
//   - Auto-degrades to 0 draws on the Noop backend (mirrors
//     PostProcessPass::execute guard) so headless tests stay clean.
//
// Scope-shrink from full ShadowPass design (deferred):
//   - No Phoskia shadow_caster program (skin, alpha-clip, etc.)
//     — see design.md §8.3.4 cut 1 → cut 2 ordering.
//   - No cascade / cube shadow / shadow bias / PCF filtering.
//   - No scene-AABB fit — identity light xform only.
//   - No Light struct on RenderScene (deferred to cut 2 per
//     `docs/execution-plan.md` §5.3; the segfault around adding
//     Light + FrameContext::ShadowMap + non-const Frame is documented
//     in §5 and isolation-tested per §5.4).
class ShadowPass : public RenderPass {
public:
    static constexpr uint16_t kDefaultShadowMapSize = 1024;

    ShadowPass() = default;
    ~ShadowPass() override;

    std::string_view name() const override { return "Shadow"; }

    uint32_t execute(PassExecContext& ctx) override;

    // R5+ — true when the pass has a live shadow FBO. Tests use
    // this to pin the slot behavior; hosts can also use it to skip
    // downstream shadow sampling when no shadow was rendered.
    bool isReady() const noexcept { return bgfx::isValid(_shadowFbo); }

    // R5+ — host can override the shadow map size before pipeline
    // dispatch (set before Renderer::render). Default = 1024.
    void setShadowMapSize(uint16_t size) noexcept { _requestedSize = size; }
    uint16_t shadowMapSize() const noexcept { return _requestedSize; }

    void destroyResources(BGFXAdapter& adapter);

private:
    void ensureShadowFbo(BGFXAdapter& adapter, uint16_t size);

    bgfx::FrameBufferHandle    _shadowFbo        = BGFX_INVALID_HANDLE;
    uint16_t                   _shadowSize       = 0;
    uint16_t                   _requestedSize    = kDefaultShadowMapSize;
};

} // namespace ayt::render::detail
