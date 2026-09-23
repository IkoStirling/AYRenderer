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
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/PresentPass.h"
#include "detail/RenderViewOrder.h"
#include "detail/SMAAPass.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
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

bool smaaFileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

uint64_t smaaFnv1a64(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

TEST_SUITE(AYRenderer_SMAA)

TEST_CASE(smaa_append_only_abi_resources_and_views_are_locked)
{
    CHECK(static_cast<uint8_t>(RenderPassSlot::SMAA) == 18u);
    CHECK(static_cast<uint8_t>(FgResourceId::SmaaEdges) == 9u);
    CHECK(static_cast<uint8_t>(FgResourceId::SmaaBlendWeights) == 10u);
    CHECK(static_cast<uint8_t>(FgResourceId::SmaaColor) == 11u);
    CHECK(static_cast<uint8_t>(FgResourceId::Count) == 26u);
    CHECK(ayt::render::detail::SMAAPass::kEdgeViewId == 247u);
    CHECK(ayt::render::detail::SMAAPass::kBlendWeightViewId == 248u);
    CHECK(ayt::render::detail::SMAAPass::kNeighborhoodViewId == 249u);
    CHECK_FALSE(
        ayt::render::detail::SMAAPass::kIntermediatePointSampled);
    CHECK(ayt::render::detail::SMAAPass::kAreaTextureWidth == 160u);
    CHECK(ayt::render::detail::SMAAPass::kAreaTextureHeight == 560u);
    CHECK(ayt::render::detail::SMAAPass::kSearchTextureWidth == 64u);
    CHECK(ayt::render::detail::SMAAPass::kSearchTextureHeight == 16u);
    CHECK(ayt::render::detail::SMAAPass::kMaxDiagonalSearchSteps == 8u);
    CHECK(std::is_final_v<ayt::render::detail::SMAAPass>);
}

TEST_CASE(default_pipelines_order_smaa_between_fxaa_and_color_grading)
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
        CHECK(fxaa == post + (desc.contains(RenderPassSlot::TAA) ? 2u : 1u));
        CHECK(smaa == fxaa + 1u);
        CHECK(grading == smaa + 1u);
        CHECK(present == grading + 1u);
    }
}

TEST_CASE(smaa_views_execute_in_three_stage_order_before_grading_and_present)
{
    const auto& order = ayt::render::detail::kRenderViewOrder;
    const auto fxaa = std::find(order.begin(), order.end(),
        ayt::render::detail::FXAAPass::kFxaaViewId);
    const auto edge = std::find(order.begin(), order.end(),
        ayt::render::detail::SMAAPass::kEdgeViewId);
    const auto weight = std::find(order.begin(), order.end(),
        ayt::render::detail::SMAAPass::kBlendWeightViewId);
    const auto neighborhood = std::find(order.begin(), order.end(),
        ayt::render::detail::SMAAPass::kNeighborhoodViewId);
    const auto grading = std::find(order.begin(), order.end(),
        ayt::render::detail::ColorGradingPass::kColorGradingViewId);
    const auto present = std::find(order.begin(), order.end(),
        ayt::render::detail::PresentPass::kPresentViewId);
    CHECK(edge == fxaa + 1);
    CHECK(weight == edge + 1);
    CHECK(neighborhood == weight + 1);
    CHECK(grading == neighborhood + 1);
    CHECK(present == grading + 1);
}

TEST_CASE(runtime_aa_toggles_are_mutually_exclusive_and_survive_rebuild)
{
    ayt::render::Renderer renderer;
    CHECK(renderer.fxaaEnabled());
    CHECK_FALSE(renderer.smaaEnabled());

    renderer.setSmaaEnabled(true);
    CHECK(renderer.smaaEnabled());
    CHECK_FALSE(renderer.fxaaEnabled());
    renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
    CHECK(renderer.smaaEnabled());
    CHECK_FALSE(renderer.fxaaEnabled());

    renderer.setFxaaEnabled(true);
    CHECK(renderer.fxaaEnabled());
    CHECK_FALSE(renderer.smaaEnabled());
    renderer.setFxaaEnabled(false);
    CHECK_FALSE(renderer.fxaaEnabled());
    CHECK_FALSE(renderer.smaaEnabled());
}

TEST_CASE(smaa_area_lookup_is_procedural_nonempty_and_deterministic)
{
    const std::vector<uint8_t> first =
        ayt::render::detail::generateSmaaAreaTextureRg8();
    const std::vector<uint8_t> second =
        ayt::render::detail::generateSmaaAreaTextureRg8();
    constexpr size_t expected =
        ayt::render::detail::SMAAPass::kAreaTextureWidth
        * ayt::render::detail::SMAAPass::kAreaTextureHeight * 2u;
    CHECK(first.size() == expected);
    CHECK(first == second);
    CHECK(std::any_of(first.begin(), first.end(),
                      [](uint8_t value) { return value != 0u; }));
    // Pattern zero occupies the first 16x16 block and must remain unfiltered.
    bool nullPatternIsZero = true;
    for (size_t y = 0; y < 16u; ++y) {
        for (size_t x = 0; x < 16u; ++x) {
            const size_t index = (y * 160u + x) * 2u;
            nullPatternIsZero = nullPatternIsZero
                && first[index] == 0u && first[index + 1u] == 0u;
        }
    }
    CHECK(nullPatternIsZero);

    // A canonical 45-degree staircase presents a 0.75 endpoint crossing
    // after the required quarter-pixel bilinear sample. That maps to block
    // (0, 3) and must carry a blend weight. Collapsing it to the point-sampled
    // value 1.0 maps to null block (0, 4), reproducing the invisible-pass bug.
    constexpr size_t distanceTexel = 1u;
    const size_t quarterCrossingIndex =
        (((3u * 16u + distanceTexel) * 160u) + distanceTexel) * 2u;
    const size_t collapsedCrossingIndex =
        (((4u * 16u + distanceTexel) * 160u) + distanceTexel) * 2u;
    CHECK(first[quarterCrossingIndex] != 0u
          || first[quarterCrossingIndex + 1u] != 0u);
    CHECK(first[collapsedCrossingIndex] == 0u);
    CHECK(first[collapsedCrossingIndex + 1u] == 0u);

    // The right half stores diagonal patterns; High must not silently regress
    // to the former 80x80 orthogonal-only table.
    bool diagonalHalfHasWeight = false;
    for (size_t y = 0; y < 400u && !diagonalHalfHasWeight; ++y) {
        for (size_t x = 80u; x < 160u; ++x) {
            const size_t index = (y * 160u + x) * 2u;
            if (first[index] != 0u || first[index + 1u] != 0u) {
                diagonalHalfHasWeight = true;
                break;
            }
        }
    }
    CHECK(diagonalHalfHasWeight);

    struct DiagonalReferenceSample {
        size_t x;
        size_t y;
        uint8_t r;
        uint8_t g;
    };
    // Samples from the MIT reference AreaTex.h. The engine uses analytic
    // half-plane coverage rather than the generator's 30x30 brute-force grid,
    // so allow a small quantization tolerance while locking layout/orientation.
    constexpr std::array<DiagonalReferenceSample, 9> diagonalReference = {{
        {81u, 1u, 65u, 61u}, {85u, 7u, 66u, 61u},
        {99u, 19u, 66u, 61u}, {101u, 1u, 77u, 42u},
        {125u, 7u, 11u, 82u}, {159u, 79u, 66u, 61u},
        {85u, 87u, 66u, 61u}, {125u, 167u, 0u, 138u},
        {159u, 319u, 96u, 35u},
    }};
    for (const DiagonalReferenceSample sample : diagonalReference) {
        const size_t index = (sample.y * 160u + sample.x) * 2u;
        CHECK(std::abs(static_cast<int>(first[index])
                       - static_cast<int>(sample.r)) <= 12);
        CHECK(std::abs(static_cast<int>(first[index + 1u])
                       - static_cast<int>(sample.g)) <= 12);
    }
}

TEST_CASE(smaa_search_lookup_matches_official_reference_bytes)
{
    const std::vector<uint8_t> search =
        ayt::render::detail::generateSmaaSearchTextureR8();
    constexpr size_t expected =
        ayt::render::detail::SMAAPass::kSearchTextureWidth
        * ayt::render::detail::SMAAPass::kSearchTextureHeight;
    CHECK(search.size() == expected);
    CHECK(std::find(search.begin(), search.end(), 254u) != search.end());
    CHECK(smaaFnv1a64(search) == 0x21C1FCF0AA631065ull);
}

TEST_CASE(smaa_graph_promotes_present_source_only_after_final_stage)
{
    ayt::render::detail::BGFXAdapter adapter;
    ayt::render::detail::FrameGraph fg(adapter);
    fg.beginFrame(640, 360);
    const ayt::render::detail::FgTextureDesc target{
        bgfx::TextureFormat::RGBA8,
        ayt::render::detail::FgTextureScale::Full,
        true,
        false,
        true,
    };
    fg.addResource(FgResourceId::FinalLdrColor, target);
    fg.addResource(FgResourceId::SmaaEdges, target);
    fg.addResource(FgResourceId::SmaaBlendWeights, target);
    fg.addResource(FgResourceId::SmaaColor, target);
    fg.addPass({"PostProcess", {}, {FgResourceId::FinalLdrColor}, true});
    fg.addPass({"SMAA",
                {FgResourceId::FinalLdrColor},
                {FgResourceId::SmaaEdges,
                 FgResourceId::SmaaBlendWeights,
                 FgResourceId::SmaaColor},
                true});
    fg.setResolvedSemantic(FgSemantic::PresentSource,
                           FgResourceId::FinalLdrColor);
    CHECK(fg.compile());
    fg.markProduced(FgResourceId::FinalLdrColor);
    fg.markProduced(FgResourceId::SmaaEdges);
    fg.markProduced(FgResourceId::SmaaBlendWeights);
    CHECK_FALSE(fg.producedThisFrame(FgResourceId::SmaaColor));

    fg.markProduced(FgResourceId::SmaaColor);
    fg.setResolvedSemantic(FgSemantic::PresentSource,
                           FgResourceId::SmaaColor);
    CHECK(fg.semanticProducedThisFrame(FgSemantic::PresentSource));
}

TEST_CASE(smaa_noop_backend_returns_zero_and_destroy_is_idempotent)
{
    ayt::render::detail::SMAAPass pass;
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

TEST_CASE(smaa_shader_contract_has_edge_search_area_and_neighborhood_stages)
{
    const std::string edge = ayt::render::detail::smaaEdgeFragmentScForTests();
    const std::string weight =
        ayt::render::detail::smaaWeightFragmentScForTests();
    const std::string neighborhood =
        ayt::render::detail::smaaNeighborhoodFragmentScForTests();
    CHECK(edge.find("smaaColorDelta") != std::string::npos);
    CHECK(edge.find("Official SMAA color-edge detection") != std::string::npos);
    CHECK(edge.find("smaaLuma") == std::string::npos);
    CHECK(edge.find("Local contrast adaptation") != std::string::npos);
    CHECK(edge.find("edges *= step") != std::string::npos);
    CHECK(weight.find("SAMPLER2D(areaTex, 1)") != std::string::npos);
    CHECK(weight.find("SAMPLER2D(searchTex, 2)") != std::string::npos);
    CHECK(weight.find("smaaSearchLength") != std::string::npos);
    CHECK(weight.find("smaaCalculateDiagWeights") != std::string::npos);
    CHECK(weight.find("smaaSearchDiag1") != std::string::npos);
    CHECK(weight.find("smaaSearchDiag2") != std::string::npos);
    CHECK(weight.find("smaaHorizontalCorner") != std::string::npos);
    CHECK(weight.find("smaaVerticalCorner") != std::string::npos);
    CHECK(weight.find("vec2(160.0, 560.0)") != std::string::npos);
    CHECK(weight.find("weights.ba = smaaArea") != std::string::npos);
    CHECK(neighborhood.find("centerWeights") != std::string::npos);
    CHECK(neighborhood.find("gl_FragColor = texture2D(inputColor, uv)")
          != std::string::npos);
    CHECK(ayt::render::detail::SMAAPass::kEdgeThreshold == 0.10f);
    CHECK(ayt::render::detail::SMAAPass::kMaxSearchSteps == 16u);
    CHECK(std::string(ayt::render::detail::kSmaaEdgeCacheKeyCStr)
          == "smaa1x_color_edge_v2");
    CHECK(std::string(ayt::render::detail::kSmaaWeightCacheKeyCStr)
          == "smaa1x_high_weights_v3");
    CHECK(std::string(ayt::render::detail::kSmaaNeighborhoodCacheKeyCStr)
          == "smaa1x_neighborhood_v2");
}

TEST_CASE(smaa_three_stage_shaders_compile_for_d3d11_and_d3d12)
{
    if (!smaaFileExists(AY_SHADER_SHADERC_HINT)) {
        std::cerr << "[SMAAPass test] SKIP: shaderc unavailable.\n";
        return;
    }

    ayt::shader::AYShadercDriver driver(AY_SHADER_SHADERC_HINT);
    auto compileStage = [&](const char* stage, const char* source,
                            const char* name) {
        ayt::shader::ShaderCompileRequest request;
        request.stage = stage;
        request.scSource = source;
        request.varyingdefSource =
            ayt::render::detail::smaaVaryingScForTests();
        request.platform = "windows";
        request.profile = "s_5_0";
        request.outputName = name;
        request.timeoutMs = 30000;
#ifdef AY_SHADER_BGFX_COMMON_HINT
        request.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
        request.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
        const ayt::shader::ShaderCompileResult result = driver.compile(request);
        if (!result.ok) {
            std::cerr << "[SMAAPass test] shaderc " << name
                      << " failed: " << result.stderrText << '\n';
        }
        return result.ok;
    };

    CHECK(compileStage("vertex",
        ayt::render::detail::smaaVertexScForTests(), "smaa1x_vertex"));
    CHECK(compileStage("fragment",
        ayt::render::detail::smaaEdgeFragmentScForTests(), "smaa1x_edge"));
    CHECK(compileStage("fragment",
        ayt::render::detail::smaaWeightFragmentScForTests(), "smaa1x_weight"));
    CHECK(compileStage("fragment",
        ayt::render::detail::smaaNeighborhoodFragmentScForTests(),
        "smaa1x_neighborhood"));
}

TEST_SUITE_END
