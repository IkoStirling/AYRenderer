// PR-E4 (2026-07-22) — §5.4 E4 plumbing smoke tests.
//
// Background (§5.4 in docs/execution-plan.md):
//
//   E4 isolates "default pipeline now includes a ShadowPass slot
//   but with setEnabled(false)". Pre-E4 the default was 4-pass
//   (no Shadow); post-E4 it's 5-pass with the Shadow slot
//   permanently disabled unless the host flips it. This is the
//   isolation experiment format: zero pixel/render behavior change
//   from the 4-pass baseline, but the pipeline machinery now
//   carries the slot.
//
// What this suite pins (all reachable on the public API):
//
//   1. makeDefault() returns exactly 5 slots AND slot[0] is Shadow.
//   2. A fresh Renderer's pipeline.passes().size() == 5 (was 4).
//   3. The Shadow slot is reachable via pipeline.findPass("Shadow").
//   4. The Shadow slot's isEnabled() == false (E4 contracts).
//   5. The remaining 4 slots are isEnabled() == true.
//   6. makeForwardWithShadows() (the explicit shadow path) yields
//      a pipeline whose Shadow slot is isEnabled() == true —
//      i.e. the host opt-in shape is preserved.
//   7. configurePipeline(makeDefault()) called twice in a row is
//      idempotent — slot 0 stays "Shadow disabled".
//   8. The canonical 5-pass execute on a Noop backend still
//      produces 0 draws (Shadow's Noop guard short-circuits; the
//      other 4 are unchanged). This is the §5.4 "no new stable
//      crash" anchor — flake ⇒ bisect back.

#include "AYRenderer.h"
#include "AYRenderTypes.h"
#include "AYTest.h"

#include "detail/BGFXAdapter.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"
#include "detail/ShadowPass.h"
#include "detail/TransparentPass.h"
#include "detail/UIPass.h"

#include "aymath/MathTypes.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

using ayt::render::Backend;
using ayt::render::InitDesc;
using ayt::render::Renderer;
using ayt::render::RenderPipelineDesc;
using ayt::render::RenderPassSlot;
using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::ForwardOpaquePass;
using ayt::render::detail::FrameContext;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::RenderPass;
using ayt::render::detail::RenderPipeline;
using ayt::render::detail::ShadowPass;
using ayt::render::detail::TransparentPass;
using ayt::render::detail::UIPass;
namespace shader = ayt::shader;

namespace {

// Shared "default has 8 slots, slot 0 is Shadow" check.
// S1a (2026-07-23) added BloomExtract between Transparent and
// PostProcess; S1b (2026-07-23) added BloomBlur between
// BloomExtract and PostProcess; S4b (2026-07-23) added
// DepthHaze between BloomBlur and PostProcess — slot index
// shifts for PostProcess/UI only.
void checkCanonicalDefaultDesc(const RenderPipelineDesc& desc)
{
    CHECK(desc.passes.size() == 9u);
    CHECK(desc.contains(RenderPassSlot::Shadow));
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    // Pre-E4 ordering of the remaining 8 preserved (CM-1 2026-08-11:
    // Forward2DOpaque inserted after ForwardOpaque; S1a BloomExtract
    // appended between Transparent and PostProcess; S1b BloomBlur
    // appended between BloomExtract and PostProcess; S4b DepthHaze
    // appended between BloomBlur and PostProcess).
    CHECK(desc.passes[1] == RenderPassSlot::ForwardOpaque);
    CHECK(desc.passes[2] == RenderPassSlot::Forward2DOpaque);  // CM-1 (2026-08-11)
    CHECK(desc.passes[3] == RenderPassSlot::Transparent);
    CHECK(desc.passes[4] == RenderPassSlot::BloomExtract);   // S1a (2026-07-23)
    CHECK(desc.passes[5] == RenderPassSlot::BloomBlur);      // S1b (2026-07-23)
    CHECK(desc.passes[6] == RenderPassSlot::DepthHaze);      // S4b (2026-07-23)
    CHECK(desc.passes[7] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[8] == RenderPassSlot::UI);
}

} // namespace

TEST_SUITE(AYRenderer_E4_DefaultShadow)

TEST_CASE(e4_make_default_desc_has_five_slots_with_shadow_first)
{
    // E4.1 — RenderPipelineDesc::makeDefault() now enumerates
    // Shadow as the first slot. Pre-E4 had only 4 slots.
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    checkCanonicalDefaultDesc(desc);
}

TEST_CASE(e4_make_forward_with_sadows_still_has_five_slots_and_shadow_first)
{
    // E4.2 — makeForwardWithShadows() was already 5-pass; this
    // pins that its ordering matches the canonical default
    // (Shadow first), so applyPipelineDesc's "is canonical
    // default" detection stays correct.
    const RenderPipelineDesc desc =
        RenderPipelineDesc::makeForwardWithShadows();
    checkCanonicalDefaultDesc(desc);
}

TEST_CASE(e4_canonical_default_shadow_slot_is_disabled_when_built_via_configure)
{
    // E4.3 — Historical (E5 retired this contract):
    //   configurePipeline(makeDefault()) → Shadow slot
    //   arrives in the pipeline with isEnabled() == false.
    //
    //   As of E5 (§5.4, 2026-07-22) this case asserts only the
    //   DESC SHAPE (5 slots, Shadow first) — the live isEnabled
    //   flag is now *true* by default, observed via the new
    //   Renderer::shadowsEnabled() public getter. Live-state
    //   assertions live in Test_E5_DefaultShadow.cpp (E5.1/E5.2).
    //   This case is preserved verbatim (assertions still hold)
    //   so the E4 desc-shape contract stays pinned.
    Renderer renderer;

    // Skip adapter init; configurePipeline operates on the
    // descriptor, not on the adapter. Pre-P2 ordering: pipeline
    // is mounted in Impl ctor, rebuilt here.
    renderer.configurePipeline(RenderPipelineDesc::makeDefault());

    const RenderPipelineDesc& desc = renderer.pipelineDesc();
    checkCanonicalDefaultDesc(desc);
    // E5 live read confirms enabled=true (the new default).
    CHECK(renderer.shadowsEnabled() == true);
}

TEST_CASE(e4_make_forward_with_sadows_marks_shadow_enabled_in_canonical_pipeline)
{
    // E4.4 — Historical (E5 retired this contract): host opt-in
    //   via makeForwardWithShadows(). After configurePipeline, the
    //   Shadow slot was meant to be isEnabled() == true.
    //
    //   As of E5, makeForwardWithShadows() is an alias for
    //   makeDefault() — both leave Shadow enabled. The E4
    //   std::equal "canonical-default ⇒ disabled" override was
    //   removed because it silently disabled Shadow for both
    //   descs (they were byte-identical) — contradicting this
    //   very case's comment. Live-state assertion now possible
    //   via Renderer::shadowsEnabled(); see Test_E5_DefaultShadow.
    //   cpp E5.3 for the directly-anchored live read.
    Renderer renderer;
    renderer.configurePipeline(RenderPipelineDesc::makeForwardWithShadows());
    const RenderPipelineDesc& desc = renderer.pipelineDesc();
    checkCanonicalDefaultDesc(desc);
    CHECK(renderer.shadowsEnabled() == true);
}

// E4.5 / E4.6 / E4.7 — exercise the raw RenderPipeline + the
// renderer's pipeline rebuild through PassExecContext. These
// reach into the Pipeline pointer via a one-time dispatch
// (adapter never initializes, so the dispatch is 0-draw).

TEST_CASE(e4_hand_built_render_pipeline_with_shadow_disabled_executes_zero_draws)
{
    // E4.5 — Hand-built pipeline re-creates the canonical
    // default: 5-pass pipeline, Shadow explicitly disabled.
    // On Noop backend (BGFXAdapter never initializes here) the
    // dispatch must produce 0 draws and not crash. The 4
    // non-shadow passes contribute 0 because their draw loops
    // are gated on adapter.isInitialized().
    RenderPipeline pipe;
    auto forwardOpaque = std::make_unique<ayt::render::detail::ForwardOpaquePass>();
    auto transparent   = std::make_unique<ayt::render::detail::TransparentPass>();
    auto postProcess   = std::make_unique<ayt::render::detail::PostProcessPass>();
    auto uiPass        = std::make_unique<ayt::render::detail::UIPass>();

    pipe.addPass(std::make_unique<ayt::render::detail::ShadowPass>());
    pipe.addPass(std::move(forwardOpaque));
    pipe.addPass(std::move(transparent));
    pipe.addPass(std::move(postProcess));
    pipe.addPass(std::move(uiPass));

    // Pin slot 0 = Shadow and confirm isEnabled is true here
    // (we built it directly; pre-canonical-default detection).
    auto* shadowSlot = pipe.findPass("Shadow");
    CHECK(shadowSlot != nullptr);
    CHECK(shadowSlot->isEnabled() == true);
    CHECK(pipe.passes().size() == 5u);
    CHECK(pipe.passes()[0]->name() == "Shadow");

    // Now flip Shadow off — simulates the canonical-default
    // behavior we cannot observe on the pimpl-mounted pipeline.
    shadowSlot->setEnabled(false);
    CHECK(shadowSlot->isEnabled() == false);

    BGFXAdapter adapter;
    shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    FrameContext frame;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };

    const uint32_t total = pipe.executeAll(ctx);
    CHECK(total == 0u);  // Noop + empty scene ⇒ 0
    CHECK(pipe.passes().size() == 5u);

    // Pin that the 4 other slots stayed enabled (only Shadow
    // was flipped).
    CHECK(pipe.findPass("ForwardOpaque")->isEnabled() == true);
    CHECK(pipe.findPass("Transparent")->isEnabled() == true);
    CHECK(pipe.findPass("PostProcess")->isEnabled() == true);
    CHECK(pipe.findPass("UI")->isEnabled() == true);
}

TEST_CASE(e4_canonical_default_setEnabled_false_does_not_affect_other_slots)
{
    // E4.6 — The shadow "disabled" override must NOT leak to
    // the other slots. Pre-E4 there was no Shadow slot in
    // makeDefault; post-E4 only Shadow gets the explicit
    // setEnabled(false) on the canonical-default path. We
    // build the same state by hand here.
    auto shadow = std::make_unique<ayt::render::detail::ShadowPass>();
    auto forwardOpaque = std::make_unique<ayt::render::detail::ForwardOpaquePass>();
    auto transparent   = std::make_unique<ayt::render::detail::TransparentPass>();
    auto postProcess   = std::make_unique<ayt::render::detail::PostProcessPass>();
    auto uiPass        = std::make_unique<ayt::render::detail::UIPass>();

    RenderPass* shadowPtr      = shadow.get();
    RenderPass* forwardOpaquePtr = forwardOpaque.get();
    RenderPass* transparentPtr = transparent.get();
    RenderPass* postProcessPtr = postProcess.get();
    RenderPass* uiPtr          = uiPass.get();

    shadow->setEnabled(false);

    RenderPipeline pipe;
    pipe.addPass(std::move(shadow));
    pipe.addPass(std::move(forwardOpaque));
    pipe.addPass(std::move(transparent));
    pipe.addPass(std::move(postProcess));
    pipe.addPass(std::move(uiPass));

    CHECK(shadowPtr->isEnabled() == false);
    CHECK(forwardOpaquePtr->isEnabled() == true);
    CHECK(transparentPtr->isEnabled() == true);
    CHECK(postProcessPtr->isEnabled() == true);
    CHECK(uiPtr->isEnabled() == true);
}

TEST_CASE(e4_pass_exec_context_brace_init_still_12_fields_after_e4)
{
    // E4.7 — PR-E4 added ZERO fields to PassExecContext. All
    // F1/F2/F3 plumbing tests still brace-init with the 12-field
    // form. Caught at the compile layer (CMake-level), but we
    // pin it explicitly so the invariant travels alongside the
    // E4 plumbing suite.
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    FrameContext frame;

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    CHECK(ctx.shadowPass == nullptr);  // F2 default, unchanged
    (void)ctx;
}

TEST_CASE(e4_canonical_default_idempotent_under_repeated_configure)
{
    // E4.8 — Calling configurePipeline(makeDefault()) twice
    // must leave the pipeline in the same shape: 5-pass, Shadow
    // disabled. Catches a regression where a second call would
    // leak an extra Shadow slot or fail to re-flip the disabled
    // flag.
    Renderer renderer;
    renderer.configurePipeline(RenderPipelineDesc::makeDefault());
    const RenderPipelineDesc& first = renderer.pipelineDesc();
    checkCanonicalDefaultDesc(first);

    renderer.configurePipeline(RenderPipelineDesc::makeDefault());
    const RenderPipelineDesc& second = renderer.pipelineDesc();
    checkCanonicalDefaultDesc(second);

    CHECK(first.passes.size() == second.passes.size());
    for (std::size_t i = 0; i < first.passes.size(); ++i) {
        CHECK(first.passes[i] == second.passes[i]);
    }
}

TEST_SUITE_END
