#include "AYTest.h"

#include "AYRenderer.h"
#include "AYRenderer/DepthHazeShaderSources.h"
#include "AYRenderer/PbrShaderSources.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "detail/DepthHazePass.h"
#include "detail/DepthHazePipeline.h"
#include "detail/FgResource.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderViewOrder.h"
#include "detail/SSAOPass.h"

#include <limits>
#include <iostream>
#include <string>

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::detail::DepthHazePass;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::SSAOPass;

namespace
{

bool compileFullscreenMaterial(const char* source, const char* label)
{
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
#ifdef AY_SHADER_SHADERC_HINT
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
#endif
    backend.platform = "linux";
    backend.profile = "430";
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif

    ayt::shader::CompiledShaderProgram program;
    compiler.compileToProgram(source, frontend, backend, program);
    if (!program.success) {
        std::cerr << '[' << label << "] compile failed:\n";
        for (const std::string& error : program.errors) {
            std::cerr << "  " << error << '\n';
        }
    }
    return program.success;
}

} // namespace

TEST_SUITE(AYRenderer_DepthHazePass_S4c)

TEST_CASE(depth_haze_parameters_are_sanitized) {
    using namespace ayt::render::detail;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CHECK(sanitizeDepthHazeStrength(-1.0f) == 0.0f);
    CHECK(sanitizeDepthHazeStrength(2.0f) == 1.0f);
    CHECK(sanitizeDepthHazeStrength(nan) == 0.0f);
    CHECK(sanitizeDepthHazeDensity(-1.0f) == 0.0f);
    CHECK(sanitizeDepthHazeDensity(inf) == 0.0f);

    const auto color = sanitizeDepthHazeColor({nan, -1.0f, 2.0f});
    CHECK(color.x == 0.0f);
    CHECK(color.y == 0.0f);
    CHECK(color.z == 2.0f);
}

TEST_CASE(depth_haze_complete_chain_gate_fails_closed) {
    using ayt::render::detail::selectDepthHazeStage;
    CHECK(selectDepthHazeStage(true, 1.0f, true, true,
                               true, true, 1280, 720));
    CHECK_FALSE(selectDepthHazeStage(false, 1.0f, true, true,
                                     true, true, 1280, 720));
    CHECK_FALSE(selectDepthHazeStage(true, 0.0f, true, true,
                                     true, true, 1280, 720));
    CHECK_FALSE(selectDepthHazeStage(true, 1.0f, false, true,
                                     true, true, 1280, 720));
    CHECK_FALSE(selectDepthHazeStage(true, 1.0f, true, false,
                                     true, true, 1280, 720));
    CHECK_FALSE(selectDepthHazeStage(true, 1.0f, true, true,
                                     false, true, 1280, 720));
    CHECK_FALSE(selectDepthHazeStage(true, 1.0f, true, true,
                                     true, false, 1280, 720));
    CHECK_FALSE(selectDepthHazeStage(true, 1.0f, true, true,
                                     true, true, 0, 720));
}

TEST_CASE(depth_haze_production_shader_uses_coverage_without_reapplying_ssao) {
    const std::string source(
        ayt::render::detail::depthHazePhoskiaSourceForTests());
    CHECK(source.find("texture2d geometryCoverage") != std::string::npos);
    CHECK(source.find("mix(1.0, surfaceFog, coverage)") != std::string::npos);
    CHECK(source.find("ssaoTexture") == std::string::npos);
    CHECK(source.find("ssaoStrength") == std::string::npos);
    CHECK(source.find("mix(raw.xyz, hazeColor.xyz, fogFactor)")
          != std::string::npos);
}

TEST_CASE(depth_haze_and_postprocess_production_shaders_compile) {
    CHECK(compileFullscreenMaterial(
        ayt::render::kDepthHazePhoskiaSource, "depth haze"));
    CHECK(compileFullscreenMaterial(
        ayt::render::detail::postProcessPhoskiaSourceForTests(),
        "post process"));
    CHECK(compileFullscreenMaterial(
        ayt::render::detail::postProcessFallbackPhoskiaSourceForTests(),
        "post process fallback"));
}

TEST_CASE(depth_haze_cache_key_and_resource_id_are_stable) {
    CHECK(std::string(ayt::render::detail::kDepthHazeCacheKeyCStr)
          == "depthhaze_v4_fullres_coverage_fs");
    CHECK(static_cast<unsigned>(FgResourceId::HazeColor) == 4u);
    CHECK(static_cast<unsigned>(FgResourceId::HazeHalf) == 4u);
}

TEST_CASE(postprocess_consumes_haze_as_primary_source_not_halfres_sampler) {
    const std::string primary(
        ayt::render::detail::postProcessPhoskiaSourceForTests());
    const std::string fallback(
        ayt::render::detail::postProcessFallbackPhoskiaSourceForTests());
    CHECK(primary.find("texture2d hazeTexture") == std::string::npos);
    CHECK(primary.find("hazeSample") == std::string::npos);
    CHECK(fallback.find("texture2d hazeTexture") == std::string::npos);
    CHECK(primary.find("ssaoTexture") == std::string::npos);
    CHECK(fallback.find("ssaoTexture") == std::string::npos);
    CHECK(std::string(ayt::render::detail::kPostProcessCacheKeyCStr)
          == "postprocess_tonemap_aces_v12_sanitized_params_fs");
}

TEST_CASE(depth_haze_and_ssao_latches_default_and_reset_false) {
    DepthHazePass haze;
    SSAOPass ssao;
    CHECK_FALSE(haze.producedThisFrame());
    CHECK_FALSE(ssao.producedThisFrame());
    haze.resetFrameState();
    ssao.resetFrameState();
    CHECK_FALSE(haze.producedThisFrame());
    CHECK_FALSE(ssao.producedThisFrame());
}

TEST_CASE(deferred_pipeline_matches_data_dependencies) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.passes.size() == 12u);
    CHECK(desc.passes[2] == RenderPassSlot::GBuffer);
    CHECK(desc.passes[3] == RenderPassSlot::SSAO);
    CHECK(desc.passes[4] == RenderPassSlot::Lighting);
    CHECK(desc.passes[5] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[6] == RenderPassSlot::Transparent);
    CHECK(desc.passes[7] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[8] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[9] == RenderPassSlot::PostProcess);
}

TEST_CASE(explicit_view_order_matches_deferred_data_dependencies) {
    const auto& order = ayt::render::detail::kRenderViewOrder;
    CHECK(order.size() == 26u);
    CHECK(order[16] == 14u);  // SSAO
    CHECK(order[17] == 8u);   // Lighting
    CHECK(order[18] == 13u);  // DepthHaze
    CHECK(order[19] == 9u);   // Deferred Transparent
    CHECK(order[20] == 10u);  // BloomExtract
    CHECK(order[21] == 11u);  // BloomBlurH
    CHECK(order[22] == 12u);  // BloomBlurV
    CHECK(order[23] == 15u);  // PostProcess
}

TEST_CASE(forward_pipeline_keeps_haze_as_safe_noop_before_transparent) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.passes.size() == 9u);
    CHECK(desc.passes[3] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[4] == RenderPassSlot::Transparent);
    CHECK(desc.passes[5] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[6] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[7] == RenderPassSlot::PostProcess);
}

TEST_CASE(builtin_pbr_transparent_shader_has_fragment_depth_haze) {
    const std::string source(ayt::render::kPbrPhoskiaSource);
    CHECK(source.find("uniform vec4 depthHaze") != std::string::npos);
    CHECK(source.find("let hazeDistance = length(worldPos - cameraPos.xyz)")
          != std::string::npos);
    CHECK(source.find("let additiveHazed = color * (1.0 - transparentFog)")
          != std::string::npos);
}

TEST_SUITE_END
