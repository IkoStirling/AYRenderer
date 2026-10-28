#include "AYTest.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"

#include <bgfx/bgfx.h>

using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FgCompileErrorCode;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgSemantic;
using ayt::render::detail::FgTextureScale;
using ayt::render::detail::FrameGraph;

namespace {

bgfx::FrameBufferHandle fakeHandle(uint16_t index)
{
    bgfx::FrameBufferHandle handle{};
    handle.idx = index;
    return handle;
}

void addFullColor(FrameGraph& graph, FgResourceId id)
{
    graph.addResource(id,
                      {bgfx::TextureFormat::RGBA8,
                       FgTextureScale::Full,
                       /*transient=*/true,
                       /*withDepth=*/false});
}

} // namespace

TEST_SUITE(AYRenderer_FrameGraph_R6)

TEST_CASE(compile_rejects_undeclared_reads)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.beginFrame(1280, 720);
    graph.addPass({"Broken", {FgResourceId::BloomBright}, {}, true});

    CHECK_FALSE(graph.compile());
    CHECK(graph.compileErrors().size() == 1u);
    CHECK(graph.compileErrors()[0].code == FgCompileErrorCode::UndeclaredRead);
    CHECK(graph.compileErrors()[0].passIndex == 0u);
    CHECK(graph.compileErrors()[0].resource == FgResourceId::BloomBright);
}

TEST_CASE(compile_rejects_read_before_owned_producer)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.beginFrame(1280, 720);
    addFullColor(graph, FgResourceId::BloomBright);
    addFullColor(graph, FgResourceId::BloomBlurA);
    graph.addPass({"Consumer",
                   {FgResourceId::BloomBright},
                   {FgResourceId::BloomBlurA},
                   true});
    graph.addPass({"LateProducer", {}, {FgResourceId::BloomBright}, true});

    CHECK_FALSE(graph.compile());
    CHECK(graph.compileErrors().size() == 1u);
    CHECK(graph.compileErrors()[0].code == FgCompileErrorCode::ReadBeforeWrite);
    CHECK(graph.compileErrors()[0].passIndex == 0u);
}

TEST_CASE(compile_rejects_multiple_writers_for_one_logical_resource)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.beginFrame(1280, 720);
    addFullColor(graph, FgResourceId::BloomBright);
    graph.addPass({"First", {}, {FgResourceId::BloomBright}, true});
    graph.addPass({"Second", {}, {FgResourceId::BloomBright}, true});

    CHECK_FALSE(graph.compile());
    CHECK(graph.compileErrors().size() == 1u);
    CHECK(graph.compileErrors()[0].code == FgCompileErrorCode::MultipleWriters);
    CHECK(graph.compileErrors()[0].passIndex == 1u);
}

TEST_CASE(terminal_pass_culls_disconnected_enabled_branch)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.beginFrame(1280, 720);
    graph.importExternal(FgResourceId::SceneColor, fakeHandle(0x20));
    addFullColor(graph, FgResourceId::FinalLdrColor);
    addFullColor(graph, FgResourceId::BloomBright);
    graph.addPass({"PostProcess",
                   {FgResourceId::SceneColor},
                   {FgResourceId::FinalLdrColor},
                   true});
    graph.addPass({"Disconnected",
                   {FgResourceId::SceneColor},
                   {FgResourceId::BloomBright},
                   true});
    graph.addPass({"Present", {FgResourceId::FinalLdrColor}, {}, true});

    CHECK(graph.compile());
    CHECK(graph.stats().declaredPasses == 3u);
    CHECK(graph.stats().livePasses == 2u);
    CHECK(graph.stats().physicalTargets == 1u);
    graph.markProduced(FgResourceId::BloomBright);
    CHECK_FALSE(graph.producedThisFrame(FgResourceId::BloomBright));
}

TEST_CASE(semantic_output_keeps_out_of_graph_consumer_branch_live)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.beginFrame(1280, 720);
    graph.importExternal(FgResourceId::SceneColor, fakeHandle(0x30));
    addFullColor(graph, FgResourceId::SSAOTexture);
    addFullColor(graph, FgResourceId::FinalLdrColor);
    graph.addPass({"SSAO", {}, {FgResourceId::SSAOTexture}, true});
    graph.setResolvedSemantic(FgSemantic::SSAOSource,
                              FgResourceId::SSAOTexture);
    graph.addPass({"PostProcess",
                   {FgResourceId::SceneColor},
                   {FgResourceId::FinalLdrColor},
                   true});
    graph.addPass({"Present", {FgResourceId::FinalLdrColor}, {}, true});

    CHECK(graph.compile());
    CHECK(graph.stats().livePasses == 3u);
    CHECK(graph.stats().physicalTargets == 2u);
}

TEST_CASE(imported_temporal_target_may_be_written_and_presented)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.beginFrame(1280, 720);
    graph.importExternal(FgResourceId::SceneColor, fakeHandle(0x40));
    graph.importExternal(FgResourceId::TaaColor, fakeHandle(0x41));
    addFullColor(graph, FgResourceId::FinalLdrColor);
    graph.addPass({"PostProcess",
                   {FgResourceId::SceneColor},
                   {FgResourceId::FinalLdrColor},
                   true});
    graph.addPass({"TAA",
                   {FgResourceId::FinalLdrColor},
                   {FgResourceId::TaaColor},
                   true});
    graph.addPass({"Present", {FgResourceId::TaaColor}, {}, true});

    CHECK(graph.compile());
    CHECK(graph.compileErrors().empty());
    CHECK(graph.stats().livePasses == 3u);
    CHECK(graph.stats().physicalTargets == 1u);
}

TEST_CASE(compile_rejects_semantic_without_owned_producer)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.beginFrame(1280, 720);
    addFullColor(graph, FgResourceId::BloomBright);
    graph.setResolvedSemantic(FgSemantic::BloomSource,
                              FgResourceId::BloomBright);

    CHECK_FALSE(graph.compile());
    CHECK(graph.compileErrors().size() == 1u);
    CHECK(graph.compileErrors()[0].code
          == FgCompileErrorCode::MissingSemanticProducer);
}

TEST_CASE(compiled_liveness_becomes_a_pipeline_execution_decision)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.beginFrame(1280, 720);
    graph.importExternal(FgResourceId::SceneColor, fakeHandle(0x50));
    addFullColor(graph, FgResourceId::FinalLdrColor);
    addFullColor(graph, FgResourceId::FxaaColor);
    graph.setExecutionEligibility(ayt::render::RenderPassSlot::PostProcess,
                                  true);
    graph.setExecutionEligibility(ayt::render::RenderPassSlot::FXAA, true);
    graph.setExecutionEligibility(ayt::render::RenderPassSlot::Present, true);
    graph.addPass({"PostProcess", {FgResourceId::SceneColor},
                   {FgResourceId::FinalLdrColor}, true, false,
                   static_cast<int16_t>(ayt::render::RenderPassSlot::PostProcess)});
    graph.addPass({"DisconnectedFxaa", {FgResourceId::FinalLdrColor},
                   {FgResourceId::FxaaColor}, true, false,
                   static_cast<int16_t>(ayt::render::RenderPassSlot::FXAA)});
    graph.addPass({"Present", {FgResourceId::FinalLdrColor}, {}, true, true,
                   static_cast<int16_t>(ayt::render::RenderPassSlot::Present)});

    CHECK(graph.compile());
    CHECK(graph.shouldExecute(ayt::render::RenderPassSlot::PostProcess));
    CHECK_FALSE(graph.shouldExecute(ayt::render::RenderPassSlot::FXAA));
    CHECK(graph.shouldExecute(ayt::render::RenderPassSlot::Present));
}

TEST_CASE(inactive_owned_resources_expire_after_a_controlled_grace_period)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.setIdleReleaseFrames(2);

    graph.beginFrame(1280, 720);
    addFullColor(graph, FgResourceId::BloomBright);
    graph.addPass({"Bloom", {}, {FgResourceId::BloomBright}, true});
    CHECK(graph.compile());
    CHECK(graph.retentionStats().retainedResources == 1u);
    CHECK(graph.retentionStats().idleResources == 0u);

    graph.beginFrame(1280, 720);
    CHECK(graph.compile());
    CHECK(graph.retentionStats().retainedResources == 1u);
    CHECK(graph.retentionStats().idleResources == 1u);

    graph.beginFrame(1280, 720);
    CHECK(graph.compile());
    CHECK(graph.retentionStats().retainedResources == 0u);
    CHECK(graph.retentionStats().releaseAfterFrames == 2u);
}

TEST_CASE(reenabled_resource_cancels_pending_idle_release)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    graph.setIdleReleaseFrames(2);

    graph.beginFrame(1280, 720);
    addFullColor(graph, FgResourceId::BloomBright);
    graph.addPass({"Bloom", {}, {FgResourceId::BloomBright}, true});
    CHECK(graph.compile());

    graph.beginFrame(1280, 720);
    CHECK(graph.compile());
    CHECK(graph.retentionStats().idleResources == 1u);

    graph.beginFrame(1280, 720);
    addFullColor(graph, FgResourceId::BloomBright);
    graph.addPass({"Bloom", {}, {FgResourceId::BloomBright}, true});
    CHECK(graph.compile());
    CHECK(graph.retentionStats().retainedResources == 1u);
    CHECK(graph.retentionStats().idleResources == 0u);
}

TEST_CASE(imported_previous_contents_do_not_depend_on_a_later_current_frame_writer)
{
    using ayt::render::RenderPassSlot;
    BGFXAdapter adapter;FrameGraph graph(adapter);
    graph.beginFrame(64,48);
    graph.importExternal(FgResourceId::FeedbackHistory,fakeHandle(70));
    addFullColor(graph,FgResourceId::FinalLdrColor);
    graph.setExecutionEligibility(RenderPassSlot::ColorGrading,true);
    graph.setExecutionEligibility(RenderPassSlot::TemporalFeedback,true);
    graph.addPass({"ReadPreviousContents",{FgResourceId::FeedbackHistory},{FgResourceId::FinalLdrColor},
        true,false,static_cast<int16_t>(RenderPassSlot::ColorGrading)});
    graph.addPass({"WriteLater",{},{FgResourceId::FeedbackHistory},
        true,false,static_cast<int16_t>(RenderPassSlot::TemporalFeedback)});
    graph.addPass({"Present",{FgResourceId::FinalLdrColor},{},true,true});
    CHECK(graph.compile());CHECK(graph.stats().livePasses==2);
    CHECK(graph.shouldExecute(RenderPassSlot::ColorGrading));
    CHECK_FALSE(graph.shouldExecute(RenderPassSlot::TemporalFeedback));
    CHECK(graph.resolve(FgResourceId::FeedbackHistory).idx==70);
}

TEST_CASE(pass_declarations_own_their_reads_and_writes_after_the_caller_changes_them)
{
    BGFXAdapter adapter;FrameGraph graph(adapter);graph.beginFrame(64,48);
    graph.importExternal(FgResourceId::SceneColor,fakeHandle(70));
    addFullColor(graph,FgResourceId::FinalLdrColor);
    ayt::render::detail::FgPassDesc descriptor{
        "Producer",{FgResourceId::SceneColor},{FgResourceId::FinalLdrColor},true};
    graph.addPass(descriptor);
    descriptor.reads.assign(1,FgResourceId::BloomBright);descriptor.writes.clear();descriptor.enabled=false;
    graph.addPass({"Present",{FgResourceId::FinalLdrColor},{},true,true});
    CHECK(graph.compile());CHECK(graph.stats().livePasses==2);CHECK(graph.stats().physicalTargets==1);
    CHECK(graph.compileErrors().empty());
}

TEST_CASE(unimported_external_handles_are_not_counted_as_current_frame_resources)
{
    BGFXAdapter adapter;FrameGraph graph(adapter);
    graph.beginFrame(64,48);
    graph.importExternal(FgResourceId::SceneColor,fakeHandle(70));
    graph.importExternal(FgResourceId::FeedbackHistory,fakeHandle(71));
    graph.addPass({"Consumer",{FgResourceId::SceneColor},{},true});
    CHECK(graph.compile());CHECK(graph.stats().logicalResources==2);
    graph.beginFrame(64,48);
    graph.importExternal(FgResourceId::SceneColor,fakeHandle(72));
    graph.addPass({"Consumer",{FgResourceId::SceneColor},{},true});
    CHECK(graph.compile());CHECK(graph.stats().logicalResources==1);
    CHECK_FALSE(graph.resourceDeclared(FgResourceId::FeedbackHistory));
    CHECK_FALSE(BGFXAdapter::isValid(graph.resolve(FgResourceId::FeedbackHistory)));
    graph.beginFrame(64,48);CHECK(graph.compile());CHECK(graph.stats().logicalResources==0);
}

TEST_SUITE_END
