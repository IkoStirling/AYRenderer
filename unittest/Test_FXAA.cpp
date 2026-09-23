#include "AYTest.h"

#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShadercDriver.h"

#include "detail/BGFXAdapter.h"
#include "detail/ColorGradingPass.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/FXAAPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/PresentPass.h"
#include "detail/RenderViewOrder.h"
#include "detail/SMAAPass.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unordered_map>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgSemantic;

std::size_t passIndex(const RenderPipelineDesc& desc, RenderPassSlot slot)
{
    const auto it = std::find(desc.passes.begin(), desc.passes.end(), slot);
    return it == desc.passes.end()
        ? desc.passes.size()
        : static_cast<std::size_t>(it - desc.passes.begin());
}

bool fxaaFileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

} // namespace

TEST_SUITE(AYRenderer_FXAA)

TEST_CASE(fxaa_append_only_abi_and_view_are_locked)
{
    CHECK(static_cast<uint8_t>(RenderPassSlot::FXAA) == 16u);
    CHECK(static_cast<uint8_t>(FgResourceId::FxaaColor) == 7u);
    CHECK(static_cast<uint8_t>(FgResourceId::Count) == 26u);
    CHECK(ayt::render::detail::FXAAPass::kFxaaViewId == 17u);
    CHECK(std::is_final_v<ayt::render::detail::FXAAPass>);
}

TEST_CASE(default_deferred_and_editor_pipelines_order_fxaa_before_present)
{
    for (const RenderPipelineDesc desc : {
             RenderPipelineDesc::makeDefault(),
             RenderPipelineDesc::makeDeferred(),
             RenderPipelineDesc::makeEditorForward(),
             RenderPipelineDesc::makeEditorDeferred()}) {
        const std::size_t post = passIndex(desc, RenderPassSlot::PostProcess);
        const std::size_t fxaa = passIndex(desc, RenderPassSlot::FXAA);
        const std::size_t smaa = passIndex(desc, RenderPassSlot::SMAA);
        const std::size_t grading = passIndex(desc, RenderPassSlot::ColorGrading);
        const std::size_t present = passIndex(desc, RenderPassSlot::Present);
        const bool deferred = desc.contains(RenderPassSlot::TAA);
        CHECK(fxaa == post + (deferred ? 2u : 1u));
        CHECK(smaa == fxaa + 1u);
        CHECK(grading == smaa + 1u);
        CHECK(present == grading + 1u);
    }
}

TEST_CASE(fxaa_view_executes_after_postprocess_and_before_present)
{
    const auto& order = ayt::render::detail::kRenderViewOrder;
    const auto pp = std::find(order.begin(), order.end(),
                              ayt::render::detail::PostProcessPass::kBlitViewId);
    const auto fxaa = std::find(order.begin(), order.end(),
                                ayt::render::detail::FXAAPass::kFxaaViewId);
    const auto grading = std::find(order.begin(), order.end(),
                                   ayt::render::detail::ColorGradingPass::kColorGradingViewId);
    const auto smaa = std::find(order.begin(), order.end(),
                                ayt::render::detail::SMAAPass::kEdgeViewId);
    const auto present = std::find(order.begin(), order.end(),
                                   ayt::render::detail::PresentPass::kPresentViewId);
    CHECK(pp != order.end());
    CHECK(fxaa == pp + 2);
    CHECK(smaa == fxaa + 1);
    CHECK(grading == smaa + 3);
    CHECK(present == grading + 1);
}

TEST_CASE(runtime_toggle_defaults_on_and_survives_pipeline_rebuild)
{
    ayt::render::Renderer renderer;
    CHECK(renderer.fxaaEnabled());
    renderer.setFxaaEnabled(false);
    CHECK_FALSE(renderer.fxaaEnabled());
    renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
    CHECK_FALSE(renderer.fxaaEnabled());
    renderer.setFxaaEnabled(true);
    CHECK(renderer.fxaaEnabled());
}

TEST_CASE(old_custom_descriptor_inserts_present_after_explicit_fxaa)
{
    ayt::render::Renderer renderer;
    RenderPipelineDesc custom{{RenderPassSlot::PostProcess,
                               RenderPassSlot::FXAA,
                               RenderPassSlot::UI}};
    renderer.configurePipeline(custom);
    const RenderPipelineDesc& resolved = renderer.pipelineDesc();
    CHECK(resolved.passes.size() == 4u);
    CHECK(resolved.passes[0] == RenderPassSlot::PostProcess);
    CHECK(resolved.passes[1] == RenderPassSlot::FXAA);
    CHECK(resolved.passes[2] == RenderPassSlot::Present);
    CHECK(resolved.passes[3] == RenderPassSlot::UI);
}

TEST_CASE(present_source_promotes_only_after_fxaa_production)
{
    ayt::render::detail::BGFXAdapter adapter;
    ayt::render::detail::FrameGraph fg(adapter);
    fg.beginFrame(640, 360);
    fg.addResource(FgResourceId::FinalLdrColor,
                   {bgfx::TextureFormat::RGBA8,
                    ayt::render::detail::FgTextureScale::Full,
                    true,
                    false});
    fg.addResource(FgResourceId::FxaaColor,
                   {bgfx::TextureFormat::RGBA8,
                    ayt::render::detail::FgTextureScale::Full,
                    true,
                    false});
    fg.addPass({"PostProcess", {}, {FgResourceId::FinalLdrColor}, true});
    fg.addPass({"FXAA",
                {FgResourceId::FinalLdrColor},
                {FgResourceId::FxaaColor},
                true});
    fg.setResolvedSemantic(FgSemantic::PresentSource,
                           FgResourceId::FinalLdrColor);
    CHECK(fg.compile());
    fg.markProduced(FgResourceId::FinalLdrColor);
    CHECK(fg.semanticProducedThisFrame(FgSemantic::PresentSource));
    CHECK_FALSE(fg.producedThisFrame(FgResourceId::FxaaColor));

    fg.markProduced(FgResourceId::FxaaColor);
    fg.setResolvedSemantic(FgSemantic::PresentSource,
                           FgResourceId::FxaaColor);
    CHECK(fg.semanticProducedThisFrame(FgSemantic::PresentSource));
}

TEST_CASE(fxaa_noop_backend_returns_zero)
{
    ayt::render::detail::FXAAPass pass;
    ayt::render::detail::BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    ayt::render::detail::FrameContext frame{};
    ayt::render::detail::PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 640, 360, frame, 0
    };
    CHECK(pass.execute(ctx) == 0u);
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
}

TEST_CASE(fxaa_quality_shader_has_contrast_gate_and_bounded_edge_search)
{
    const std::string vertex = ayt::render::detail::fxaaVertexScForTests();
    const std::string fragment = ayt::render::detail::fxaaFragmentScForTests();
    const std::string varying = ayt::render::detail::fxaaVaryingScForTests();

    CHECK(varying.find("v_texcoord0") != std::string::npos);
    CHECK(vertex.find("v_texcoord0 = a_texcoord0") != std::string::npos);
    CHECK(fragment.find("SAMPLER2D(inputColor, 0)") != std::string::npos);
    CHECK(fragment.find("uniform vec4 inverseViewport") != std::string::npos);
    CHECK(fragment.find("uniform vec4 fxaaQuality") != std::string::npos);
    CHECK(fragment.find("fxaaColorDistance") != std::string::npos);
    CHECK(fragment.find("fxaaSecondDerivative") != std::string::npos);
    CHECK(fragment.find("edgeRange < edgeThreshold") != std::string::npos);
    CHECK(fragment.find("chromaDominant") != std::string::npos);
    CHECK(fragment.find("gl_FragColor = colorM") != std::string::npos);
    CHECK(fragment.find("edgeHorizontal") != std::string::npos);
    CHECK(fragment.find("edgeVertical") != std::string::npos);
    CHECK(fragment.find("distanceNegative = 8.0") != std::string::npos);
    CHECK(fragment.find("distancePositive = 8.0") != std::string::npos);
    CHECK(fragment.find("subpixelOffset") != std::string::npos);
    CHECK(fragment.find("fxaaClampUv") != std::string::npos);

    CHECK(ayt::render::detail::FXAAPass::kEdgeThreshold == 0.125f);
    CHECK(ayt::render::detail::FXAAPass::kEdgeThresholdMin == 0.0312f);
    CHECK(ayt::render::detail::FXAAPass::kSubpixelQuality == 0.50f);
    CHECK(ayt::render::detail::FXAAPass::kSearchThreshold == 0.25f);
    CHECK(std::string(ayt::render::detail::kFxaaCacheKeyCStr)
          == "fxaa_chroma_edge_search_v3");
}

TEST_CASE(fxaa_quality_shader_compiles_for_d3d11_and_d3d12)
{
    if (!fxaaFileExists(AY_SHADER_SHADERC_HINT)) {
        std::cerr << "[FXAAPass test] SKIP: shaderc unavailable.\n";
        return;
    }

    ayt::shader::AYShadercDriver driver(AY_SHADER_SHADERC_HINT);
    auto compileStage = [&](const char* stage, const char* source) {
        ayt::shader::ShaderCompileRequest request;
        request.stage = stage;
        request.scSource = source;
        request.varyingdefSource =
            ayt::render::detail::fxaaVaryingScForTests();
        request.platform = "windows";
        // bgfx D3D11 and D3D12 both consume shader model 5 binaries.
        request.profile = "s_5_0";
        request.outputName = std::string("fxaa_quality_") + stage;
        request.timeoutMs = 30000;
#ifdef AY_SHADER_BGFX_COMMON_HINT
        request.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
        request.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
        const ayt::shader::ShaderCompileResult result = driver.compile(request);
        if (!result.ok) {
            std::cerr << "[FXAAPass test] shaderc " << stage
                      << " failed: " << result.stderrText << '\n';
        }
        return result.ok;
    };

    CHECK(compileStage(
        "vertex", ayt::render::detail::fxaaVertexScForTests()));
    CHECK(compileStage(
        "fragment", ayt::render::detail::fxaaFragmentScForTests()));
}

TEST_SUITE_END
