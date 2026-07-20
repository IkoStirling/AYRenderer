#pragma once

#include "detail/RenderPass.h"

#include <memory>
#include <vector>

namespace ayt::render::detail
{

// U1+ — owns the ordered list of RenderPass subclasses and dispatches
// them via executeAll(). Renderer::render iterates pipeline.passes()
// and calls execute() on each enabled pass. Today the pipeline holds
// [ForwardOpaque, Transparent, PostProcess, UI] (4 Pass); see
// `docs/execution-plan.md` §1.1 + §附录 B. R5+ deferred slots
// (Shadow / GBuffer / Lighting) would slot in per design.md:467-470
// kFullPipelineOrder, but are NOT in the default pipeline:
//   - ShadowPass cut-1 stub (identity light xform, depth-only FBO)
//     ships as compile-clean code but is intentionally NOT addPass'd.
//     Default-on Shadow is forbidden by `docs/execution-plan.md` §5.3
//     (segfault constraint; see §5 for isolation experiment protocol).
//   - GBufferPass / LightingPass do not exist yet.
//
// Non-ownership semantics: passes are owned via unique_ptr. The
// pipeline outlives every render() call so dispatch is safe to
// re-enter; per-frame state (binding caches on GpuMaterial) lives on
// the resources map, not on the passes themselves.
//
// Threading: executeAll() is called from the render thread (currently
// main thread; multiplayer deferred to Phase 5). Matches the
// RenderPass::execute contract.
//
// P1 (PR-C, 2026-07-20): executeAll() now takes a single
// PassExecContext& instead of 12 unpacked args. The host
// (Renderer::render) builds the context once per frame and
// every enabled pass reads from it. See PassExecContext.h.
class RenderPipeline {
public:
    // Take ownership. Order of calls determines dispatch order; later
    // insert points (R5+) should be appended in the same order
    // design.md prescribes.
    void addPass(std::unique_ptr<RenderPass> pass);

    // Non-owning access; valid until next addPass or destruction.
    const std::vector<std::unique_ptr<RenderPass>>& passes() const noexcept { return _passes; }
    std::vector<std::unique_ptr<RenderPass>>&       passes()       noexcept { return _passes; }

    // Dispatch every enabled pass in registration order. Returns sum
    // of per-pass execute() return values (used to update
    // RenderFrameStats.drawCalls).
    //
    // P1 (PR-C, 2026-07-20): takes a single PassExecContext& instead
    // of 12 unpacked args. The host builds the context once per frame
    // and every enabled pass reads from it. Threading the same
    // `materials` non-const ref through every pass is load-bearing:
    // ForwardOpaquePass lazily resolves BindingIds on first use and
    // caches them in GpuMaterial fields.
    uint32_t executeAll(PassExecContext& ctx);

private:
    std::vector<std::unique_ptr<RenderPass>> _passes;
};

} // namespace ayt::render::detail