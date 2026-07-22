// Renderer::configurePipeline rebuilds the ordered pass list from
// RenderPipelineDesc slots. Pins default vs shadow-forward assembly
// and empty-desc fallback.

#include "AYTest.h"
#include "AYRenderer.h"

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::Renderer;

TEST_SUITE(AYRenderer_PipelineConfig)

TEST_CASE(pipeline_desc_make_default_has_no_shadow) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.passes.size() == 4u);
    CHECK(!desc.contains(RenderPassSlot::Shadow));
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
    CHECK(renderer.pipelineDesc().passes.size() == 4u);
    CHECK(!renderer.pipelineDesc().contains(RenderPassSlot::Shadow));

    renderer.configurePipeline(RenderPipelineDesc::makeForwardWithShadows());
    CHECK(renderer.pipelineDesc().passes.size() == 5u);
    CHECK(renderer.pipelineDesc().passes.front() == RenderPassSlot::Shadow);
    CHECK(renderer.pipelineDesc().contains(RenderPassSlot::Shadow));

    renderer.configurePipeline(RenderPipelineDesc::makeDefault());
    CHECK(renderer.pipelineDesc().passes.size() == 4u);
    CHECK(!renderer.pipelineDesc().contains(RenderPassSlot::Shadow));

    // Empty desc falls back to default.
    renderer.configurePipeline(RenderPipelineDesc{});
    CHECK(renderer.pipelineDesc().passes.size() == 4u);
}

TEST_SUITE_END
