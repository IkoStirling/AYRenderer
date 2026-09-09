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

TEST_SUITE_END
