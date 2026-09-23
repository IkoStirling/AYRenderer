#include "AYTest.h"

#include "AYRenderer/SSAOShaderSources.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "detail/DepthHazePass.h"
#include "detail/LightingPass.h"
#include "detail/PostProcessPass.h"
#include "detail/SSAOPass.h"
#include "detail/SSAOPipeline.h"

#include <iostream>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

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

std::string readRendererSource(const char* relativePath)
{
    const std::string path =
        std::string(AY_RENDERER_SOURCE_DIR) + "/" + relativePath;
    std::ifstream file(path, std::ios::binary);
    std::stringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

} // namespace

TEST_SUITE(AYRenderer_SSAO_A3)

TEST_CASE(a3_parameters_are_sanitized)
{
    using namespace ayt::render::detail;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CHECK(sanitizeSsaoStrength(-1.0f) == 0.0f);
    CHECK(sanitizeSsaoStrength(2.0f) == 1.0f);
    CHECK(sanitizeSsaoStrength(nan) == 0.0f);
    CHECK(sanitizeSsaoRadius(-1.0f) == 0.0f);
    CHECK(sanitizeSsaoRadius(inf) == 0.0f);
    CHECK(sanitizeSsaoBias(-1.0f) == 0.0f);
    CHECK(sanitizeSsaoBias(nan) == 0.0f);
}

TEST_CASE(a3_complete_chain_gate_fails_closed)
{
    using ayt::render::detail::selectSsaoStage;
    CHECK(selectSsaoStage(true, 0.6f, 0.5f,
                          true, true, true, true, true, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(false, 0.6f, 0.5f,
                                true, true, true, true, true, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.0f, 0.5f,
                                true, true, true, true, true, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.6f, 0.0f,
                                true, true, true, true, true, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.6f, 0.5f,
                                false, true, true, true, true, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.6f, 0.5f,
                                true, false, true, true, true, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.6f, 0.5f,
                                true, true, false, true, true, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.6f, 0.5f,
                                true, true, true, false, true, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.6f, 0.5f,
                                true, true, true, true, false, true, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.6f, 0.5f,
                                true, true, true, true, true, false, 1280, 720));
    CHECK_FALSE(selectSsaoStage(true, 0.6f, 0.5f,
                                true, true, true, true, true, true, 0, 720));
}

TEST_CASE(a3_pass_parameter_gate_matches_frame_graph_allocation_gate)
{
    using ayt::render::detail::ssaoParametersActive;
    CHECK(ssaoParametersActive(true, 0.45f, 0.4f));
    CHECK_FALSE(ssaoParametersActive(false, 0.45f, 0.4f));
    CHECK_FALSE(ssaoParametersActive(true, 0.0f, 0.4f));
    CHECK_FALSE(ssaoParametersActive(true, 0.45f, 0.0f));
}

TEST_CASE(a3_production_shader_uses_rt3_coverage_tbn_and_view_depth)
{
    const std::string source(ayt::render::kSsaoPhoskiaSource);
    CHECK(source.find("texture2d geometryCoverage") != std::string::npos);
    CHECK(source.find("sample(geometryCoverage, uv).a") != std::string::npos);
    CHECK(source.find("centerWorld.w") == std::string::npos);
    CHECK(source.find("cross(helper, Nn)") != std::string::npos);
    CHECK(source.find("viewportRect.zw") != std::string::npos);
    CHECK(source.find("viewMatrix * vec4(sampleWorld0, 1.0)")
          != std::string::npos);
    CHECK(source.find("abs(actualView0.z) + bias, abs(sampleView0.z)")
          != std::string::npos);
    CHECK(source.find("dot(to0, Nn)") != std::string::npos);
    CHECK(source.find("0.01") == std::string::npos);
    CHECK(source.find("uniform vec4 ssaoStrength") == std::string::npos);
}

TEST_CASE(a3_production_shader_compiles)
{
    CHECK(compileFullscreenMaterial(ayt::render::kSsaoPhoskiaSource,
                                    "ssao"));
}

TEST_CASE(a3_lighting_applies_ssao_to_ambient_only)
{
    const std::string source(
        ayt::render::detail::kLightingPhoskiaSourceCStr);
    CHECK(source.find("texture2d ssaoTexture") != std::string::npos);
    CHECK(source.find("let ssaoAmbient") != std::string::npos);
    CHECK(source.find("materialAo * ssaoAmbient") != std::string::npos);
    CHECK(source.find("let pbrLit = ambientLit + directLit + surface.rgb")
          != std::string::npos);
    CHECK(source.find("let unlit = albedo.rgb + surface.rgb")
          != std::string::npos);
    CHECK(source.find("mix(pbrLit, unlit, isUnlit)")
          != std::string::npos);

    const std::string haze(
        ayt::render::detail::depthHazePhoskiaSourceForTests());
    const std::string post(
        ayt::render::detail::postProcessPhoskiaSourceForTests());
    const std::string postFallback(
        ayt::render::detail::postProcessFallbackPhoskiaSourceForTests());
    CHECK(haze.find("ssaoTexture") == std::string::npos);
    CHECK(post.find("ssaoTexture") == std::string::npos);
    CHECK(postFallback.find("ssaoTexture") == std::string::npos);
}

TEST_CASE(a3_cache_keys_and_latch_are_current)
{
    CHECK(std::string(ayt::render::detail::kSSAOCacheKeyCStr)
          == "ssao_v5_8tap_viewdepth_tbn_coverage_fs");
    CHECK(std::string(ayt::render::detail::kLightingCacheKeyCStr)
          == "lighting_v33_ibl_v2_split_sum");
    ayt::render::detail::SSAOPass pass;
    CHECK_FALSE(pass.isReady());
    CHECK_FALSE(pass.producedThisFrame());
    pass.resetFrameState();
    CHECK_FALSE(pass.producedThisFrame());
}

TEST_CASE(a3_ssao_resources_are_destroyed_before_pipeline_or_adapter_teardown)
{
    const std::string source = readRendererSource("src/AYRenderer.cpp");
    CHECK_FALSE(source.empty());

    const size_t reconfigure =
        source.find("void Renderer::Impl::applyPipelineDesc");
    const size_t reconfigureSsao =
        source.find("pipeline.findPass<detail::SSAOPass>()", reconfigure);
    const size_t pipelineClear = source.find("pipeline.clear();", reconfigure);
    CHECK(reconfigure != std::string::npos);
    CHECK(reconfigureSsao != std::string::npos);
    CHECK(pipelineClear != std::string::npos);
    CHECK(reconfigureSsao < pipelineClear);

    const size_t shutdown = source.find("void Renderer::shutdown()");
    const size_t shutdownSsao =
        source.find("_impl->pipeline.findPass<detail::SSAOPass>()", shutdown);
    const size_t adapterShutdown =
        source.find("_impl->adapter.shutdown()", shutdown);
    CHECK(shutdown != std::string::npos);
    CHECK(shutdownSsao != std::string::npos);
    CHECK(adapterShutdown != std::string::npos);
    CHECK(shutdownSsao < adapterShutdown);
}

TEST_SUITE_END
