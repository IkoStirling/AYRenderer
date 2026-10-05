#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/BGFXAdapter.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/RenderPassContract.h"
#include "detail/RenderViewOrder.h"
#include <limits>
using namespace ayt::render;
using namespace ayt::render::detail;
TEST_SUITE(AYRenderer_SSR)
TEST_CASE(settings_are_finite_bounded_and_default_off) {
    Renderer renderer;
    CHECK(!renderer.ssrSettings().enabled);
    SsrSettings s; s.enabled = true; s.strength = std::numeric_limits<float>::quiet_NaN();
    s.maxDistance = -2; s.steps = 255; s.debugView = 99;
    renderer.setSsrSettings(s); s = renderer.ssrSettings();
    CHECK(s.enabled); CHECK(s.strength == 1); CHECK(s.maxDistance == 0.1f);
    CHECK(s.steps == 64); CHECK(s.debugView == 2);
}
TEST_CASE(graph_adds_hdr_dependency_and_disabled_branch_is_not_live) {
    BGFXAdapter adapter; FrameGraph graph(adapter);
    PostProcessGraphPlanInput in; in.width = 64; in.height = 32;
    in.sceneColor = bgfx::FrameBufferHandle{7}; in.gbuffer = bgfx::FrameBufferHandle{8};
    in.finalLdr = true; in.ssr = true; in.haze = true;
    auto result = buildPostProcessGraphPlan(graph, in);
    CHECK(result.compileSucceeded); CHECK(result.hdrSceneSource == FgResourceId::HazeColor);
    CHECK(graph.shouldExecute(RenderPassSlot::SSR));
    in.ssr = false; result = buildPostProcessGraphPlan(graph, in);
    CHECK(result.compileSucceeded); CHECK(!graph.shouldExecute(RenderPassSlot::SSR));
    CHECK(!BGFXAdapter::isValid(graph.resolve(FgResourceId::SsrColor)));
    CHECK(validateRenderPipelineContracts(RenderPipelineDesc::makeDeferred().passes).errors.empty());
}
TEST_CASE(view_order_and_abi_are_append_only) {
    CHECK(static_cast<uint8_t>(RenderPassSlot::SSR) == 22);
    CHECK(static_cast<uint8_t>(FgResourceId::SsrColor) == 26);
    size_t light = 256, ssr = 256, haze = 256;
    for (size_t i = 0; i < kRenderViewRemap.size(); ++i) {
        if(kRenderViewRemap[i] == 8) light = i;
        if(kRenderViewRemap[i] == 32) ssr = i;
        if(kRenderViewRemap[i] == 13) haze = i;
    }
    CHECK(light < ssr); CHECK(ssr < haze);
}
}
