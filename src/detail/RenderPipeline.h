#pragma once

#include "detail/RenderPass.h"

#include <memory>
#include <string_view>
#include <vector>

namespace ayt::render::detail
{

// U1+ — owns the ordered list of RenderPass subclasses and dispatches
// them via executeAll(). Renderer::render iterates pipeline.passes()
// and calls execute() on each enabled pass. Today the default
// pipeline (built by RenderPipelineDesc::makeDefault) holds 5 passes
// with Shadow *enabled* at slot 0 (E5 §5.4, 2026-07-22):
// [Shadow, ForwardOpaque, Transparent, PostProcess, UI]. See
// `docs/execution-plan.md` §1.1 + §附录 B + 附录 A row E5. The E4
// "canonical default ⇒ Shadow disabled" override is removed.
// §5.3 still forbids default-on Shadow *combined with* a Light
// struct or FrameContext shadow writeback — both DIAG flags remain
// OFF, so neither ship-path is live. GBufferPass / LightingPass
// do not exist yet.
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
    // design.md / RenderPipelineDesc prescribe.
    void addPass(std::unique_ptr<RenderPass> pass);

    // Drop all owned passes. Callers that hold ShadowPass GPU resources
    // must destroyResources() first when the adapter is live.
    void clear();

    // Name lookup (O(N), N typically ≤ 7). Returns nullptr when absent.
    RenderPass*       findPass(std::string_view name) noexcept;
    const RenderPass* findPass(std::string_view name) const noexcept;

    // Non-owning access; valid until next addPass/clear or destruction.
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