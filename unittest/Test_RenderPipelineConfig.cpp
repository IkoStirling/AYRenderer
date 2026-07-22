// Renderer::configurePipeline rebuilds the ordered pass list from
// RenderPipelineDesc slots. Pins default vs shadow-forward assembly
// and empty-desc fallback.
//
// E4 (§5.4, 2026-07-22): makeDefault() now mounts Shadow at slot 0
// (disabled) instead of omitting it entirely. The desc-level contracts
// pinned here are the canonical-default SHAPE; whether Shadow is
// dispatched is owned by `setEnabled(false/true)` on the live
// RenderPass, not by membership in the desc. Pre-E4 the assertions
// said "no Shadow" — now they say "Shadow first, same 4 remaining
// slots in order".

#include "AYTest.h"
#include "AYRenderer.h"

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::Renderer;

TEST_SUITE(AYRenderer_PipelineConfig)

TEST_CASE(pipeline_desc_make_default_has_shadow_first_but_disabled_by_lifecycle) {
    // E4 — makeDefault() = 5 slots, Shadow first, then FO/Trans/PP/UI.
    // Whether the Shadow pass actually executes is decided at the
    // RenderPass level via setEnabled(false/true), NOT by desc
    // membership. We pin the SHAPE here; the enable-state assertion
    // lives in Test_E4_DefaultShadow.cpp.
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.passes.size() == 5u);
    CHECK(desc.contains(RenderPassSlot::Shadow));
    CHECK(desc.passes.front() == RenderPassSlot::Shadow);
    CHECK(desc.contains(RenderPassSlot::ForwardOpaque));
    CHECK(desc.contains(RenderPassSlot::Transparent));
    CHECK(desc.contains(RenderPassSlot::PostProcess));
    CHECK(desc.contains(RenderPassSlot::UI));
}

TEST_CASE(pipeline_desc_make_forward_with_shadows_orders_shadow_first) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeForwardWithShadows();
    CHECK(desc.passes.size() == 5u);
    CHECK(desc.passes.front() == RenderPassSlot::Shadow);
    CHECK(desc.contains(RenderPassSlot::Shadow));
}

TEST_CASE(renderer_configure_pipeline_rebuilds_slots) {
    Renderer renderer;
    // E4 — the canonical default mounts Shadow first (disabled).
    CHECK(renderer.pipelineDesc().passes.size() == 5u);
    CHECK(renderer.pipelineDesc().contains(RenderPassSlot::Shadow));

    renderer.configurePipeline(RenderPipelineDesc::makeForwardWithShadows());
    CHECK(renderer.pipelineDesc().passes.size() == 5u);
    CHECK(renderer.pipelineDesc().passes.front() == RenderPassSlot::Shadow);
    CHECK(renderer.pipelineDesc().contains(RenderPassSlot::Shadow));

    renderer.configurePipeline(RenderPipelineDesc::makeDefault());
    CHECK(renderer.pipelineDesc().passes.size() == 5u);
    CHECK(renderer.pipelineDesc().contains(RenderPassSlot::Shadow));

    // Empty desc falls back to default (also now 5 slots).
    renderer.configurePipeline(RenderPipelineDesc{});
    CHECK(renderer.pipelineDesc().passes.size() == 5u);
}

TEST_SUITE_END
