#include "AYTest.h"

#include "detail/BGFXAdapter.h"
#include "detail/PostProcessGraphPlan.h"

#include <bgfx/bgfx.h>

using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FgCompileErrorCode;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FrameGraph;
using ayt::render::detail::PostProcessGraphPlanInput;
using ayt::render::detail::PostProcessGraphPlanResult;
using ayt::render::detail::buildPostProcessGraphPlan;

namespace {

bgfx::FrameBufferHandle fakeHandle(uint16_t index)
{
    bgfx::FrameBufferHandle handle{};
    handle.idx = index;
    return handle;
}

PostProcessGraphPlanInput baseInput()
{
    PostProcessGraphPlanInput input{};
    input.width = 1280;
    input.height = 720;
    input.sceneColor = fakeHandle(0x70);
    input.smaaIntermediatePointSampled = true;
    return input;
}

} // namespace

TEST_SUITE(AYRenderer_PostProcessGraphPlan_R6)

TEST_CASE(no_effects_keeps_only_the_borrowed_scene_resource)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);

    const PostProcessGraphPlanResult result =
        buildPostProcessGraphPlan(graph, baseInput());

    CHECK(result.compileSucceeded);
    CHECK(graph.compileErrors().empty());
    CHECK(graph.stats().declaredPasses == 0u);
    CHECK(graph.stats().livePasses == 0u);
    CHECK(graph.stats().logicalResources == 1u);
    CHECK(graph.stats().physicalTargets == 0u);
    CHECK(result.hdrSceneSource == FgResourceId::SceneColor);
}

TEST_CASE(full_temporal_chain_preserves_every_required_dependency)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    PostProcessGraphPlanInput input = baseInput();
    input.taaWriteTarget = fakeHandle(0x71);
    input.ssao = true;
    input.haze = true;
    input.bloomExtract = true;
    input.bloomBlur = true;
    input.finalLdr = true;
    input.taa = true;
    input.colorGrading = true;

    const PostProcessGraphPlanResult result =
        buildPostProcessGraphPlan(graph, input);

    CHECK(result.compileSucceeded);
    CHECK(graph.compileErrors().empty());
    CHECK(graph.stats().declaredPasses == 9u);
    CHECK(graph.stats().livePasses == 9u);
    CHECK(graph.stats().logicalResources == 9u);
    CHECK(graph.stats().physicalTargets == 7u);
    CHECK(result.hdrSceneSource == FgResourceId::HazeColor);
    CHECK(result.antialiasingSource == FgResourceId::TaaColor);
    CHECK(result.presentationSource == FgResourceId::ColorGradedColor);
    CHECK(graph.resolve(FgResourceId::TaaColor).idx == 0x71u);
}

TEST_CASE(spatial_chain_orders_fxaa_before_smaa)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    PostProcessGraphPlanInput input = baseInput();
    input.finalLdr = true;
    input.fxaa = true;
    input.smaa = true;

    const PostProcessGraphPlanResult result =
        buildPostProcessGraphPlan(graph, input);

    CHECK(result.compileSucceeded);
    CHECK(graph.stats().declaredPasses == 4u);
    CHECK(graph.stats().livePasses == 4u);
    CHECK(graph.stats().physicalTargets == 5u);
    CHECK(result.antialiasingSource == FgResourceId::SmaaColor);
    CHECK(result.presentationSource == FgResourceId::SmaaColor);
}

TEST_CASE(temporal_priority_culls_a_disconnected_spatial_branch)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    PostProcessGraphPlanInput input = baseInput();
    input.taaWriteTarget = fakeHandle(0x72);
    input.finalLdr = true;
    input.taa = true;
    input.fxaa = true;

    const PostProcessGraphPlanResult result =
        buildPostProcessGraphPlan(graph, input);

    CHECK(result.compileSucceeded);
    CHECK(graph.stats().declaredPasses == 4u);
    CHECK(graph.stats().livePasses == 3u);
    CHECK(graph.stats().physicalTargets == 1u);
    CHECK(result.antialiasingSource == FgResourceId::TaaColor);
}

TEST_CASE(invalid_bloom_partial_chain_fails_closed_at_compile)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    PostProcessGraphPlanInput input = baseInput();
    input.bloomBlur = true;
    input.finalLdr = true;

    const PostProcessGraphPlanResult result =
        buildPostProcessGraphPlan(graph, input);

    CHECK_FALSE(result.compileSucceeded);
    CHECK_FALSE(graph.compileErrors().empty());
    CHECK(graph.compileErrors()[0].code
          == FgCompileErrorCode::UndeclaredRead);
    CHECK(graph.stats().physicalTargets == 0u);
}

TEST_SUITE_END
