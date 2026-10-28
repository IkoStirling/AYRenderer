#pragma once

#include "detail/RenderPass.h"

#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace ayt::render::detail
{

class PresentationPass;

enum class PassExecutionState : uint8_t {
    Submitted = 0,
    CompletedNoDraws,
    Disabled,
    GraphCulled,
    Failed,
    Integrated,
};

struct PassExecutionOutcome {
    std::string name;
    PassExecutionState state = PassExecutionState::CompletedNoDraws;
    uint32_t drawCalls = 0;
    std::string detail;
};

// U1+ — owns the ordered list of RenderPass subclasses and dispatches
// them via executeAll(). Renderer::render iterates pipeline.passes()
// and calls execute() on each enabled, live pass. Product composition is in
// RenderPipelineDesc.cpp and instance construction in RenderPipelineFactory.
// This class owns dispatch and observations, not product defaults or GPU View
// remapping. Shadow is mounted in both canonical product pipelines.
//
// Ownership: passes are owned via unique_ptr. The
// pipeline outlives every render() call so dispatch is safe to
// re-enter; per-frame state (binding caches on GpuMaterial) lives on
// the resources map, not on the passes themselves.
//
// Threading: executeAll() is called from the render thread (currently
// main thread). Matches the
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
    void addPass(RenderPassSlot slot, std::unique_ptr<RenderPass> pass);

    // Drop the owned CPU pass objects. Built-in GPU state must first be cleaned
    // through RenderPipelineLifecycle while its adapter is live; custom pass
    // owners provide their cleanup. A borrowed UI backend is not destroyed.
    void clear();

    // Name lookup is linear in mounted passes. Returns nullptr when absent.
    RenderPass*       findPass(std::string_view name) noexcept;
    const RenderPass* findPass(std::string_view name) const noexcept;

    // Exact-type lookup for production wiring. This removes the unsafe
    // "string lookup + unchecked static_cast" pair: a custom pass reporting a
    // built-in name can no longer be interpreted as the wrong concrete type.
    // The index is maintained when passes are registered, so lookup is O(1)
    // and duplicate concrete types preserve the first-registration behavior
    // of the legacy name lookup. The legacy lookup remains for diagnostics.
    template<class PassT>
    PassT* findPass() noexcept
    {
        static_assert(std::is_base_of_v<RenderPass, PassT>);
        const auto it = _passesByType.find(std::type_index(typeid(PassT)));
        return it != _passesByType.end()
            ? static_cast<PassT*>(it->second)
            : nullptr;
    }

    template<class PassT>
    const PassT* findPass() const noexcept
    {
        static_assert(std::is_base_of_v<RenderPass, PassT>);
        const auto it = _passesByType.find(std::type_index(typeid(PassT)));
        return it != _passesByType.end()
            ? static_cast<const PassT*>(it->second)
            : nullptr;
    }

    // Non-owning access; valid until next addPass/clear or destruction.
    const std::vector<std::unique_ptr<RenderPass>>& passes() const noexcept { return _passes; }
    // Indexed once at assembly; per-frame presentation hooks need no RTTI scan.
    // Like the type index, this borrows instances until addPass/clear/destruction.
    const std::vector<PresentationPass*>& presentationPasses() const noexcept { return _presentationPasses; }

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

    const std::vector<RenderPassFrameStats>& lastPassStats() const noexcept {
        return _lastPassStats;
    }
    const std::vector<PassExecutionOutcome>& lastPassOutcomes() const noexcept {
        return _lastPassOutcomes;
    }
    void resetFrameStats() {
        _lastPassStats.clear();
        _lastPassOutcomes.clear();
    }

private:
    void addPassImpl(int16_t slot, std::unique_ptr<RenderPass> pass);
    std::vector<std::unique_ptr<RenderPass>> _passes;
    std::vector<PresentationPass*> _presentationPasses;
    // Non-owning pointers into _passes. Moving a unique_ptr during vector
    // growth does not move its pointee. try_emplace keeps the first instance
    // if a pipeline intentionally registers the same concrete type twice.
    std::unordered_map<std::type_index, RenderPass*> _passesByType;
    // Parallel to _passes. Plain legacy/test passes remain untracked; a
    // PresentationPass infers its own declared slot even through legacy add.
    std::vector<int16_t> _passSlots;
    std::vector<RenderPassFrameStats>         _lastPassStats;
    std::vector<PassExecutionOutcome>         _lastPassOutcomes;
};

} // namespace ayt::render::detail
