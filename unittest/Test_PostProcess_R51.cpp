#include "AYTest.h"

#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "detail/PostProcessPass.h"
#include "detail/PostProcessPipeline.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace
{

void compileProductionSource(const char* source,
                             ayt::shader::CompiledShaderProgram& program)
{
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
    backend.platform = "linux";
    backend.profile = "430";
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    compiler.compileToProgram(source, frontend, backend, program);
}

void printCompileErrors(const ayt::shader::CompiledShaderProgram& program,
                        const char* variant)
{
    for (const std::string& error : program.errors) {
        std::cerr << "[R5.1 " << variant << "] " << error << '\n';
    }
}

bool hasUniform(const ayt::shader::CompiledShaderProgram& program,
                const char* name)
{
    return std::any_of(program.uniforms.begin(), program.uniforms.end(),
        [name](const ayt::shader::BGFXUniform& uniform) {
            return uniform.name == name;
        });
}

bool hasTexture(const ayt::shader::CompiledShaderProgram& program,
                const char* name)
{
    return std::any_of(program.textures.begin(), program.textures.end(),
        [name](const ayt::shader::BGFXTexture& texture) {
            return texture.name == name;
        });
}

std::string readRendererFile(const char* relativePath)
{
    std::ifstream file(std::string(AY_RENDERER_SOURCE_DIR) + "/" + relativePath);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

TEST_SUITE(AYRenderer_PostProcessPass_R51_Smoke)

TEST_CASE(r51_pass_stack_lifecycle_is_safe_across_incremental_builds)
{
    CHECK(std::is_final_v<ayt::render::detail::PostProcessPass>);
    ayt::render::detail::PostProcessPass pass;
    CHECK_FALSE(pass.isReady());
}

TEST_SUITE_END

TEST_SUITE(AYRenderer_PostProcessPass_R51)

TEST_CASE(r51_exposure_sanitizer_is_finite_and_bounded)
{
    using ayt::render::detail::sanitizePostProcessExposure;
    CHECK(sanitizePostProcessExposure(-1.0f) == 0.0f);
    CHECK(sanitizePostProcessExposure(2.5f) == 2.5f);
    CHECK(sanitizePostProcessExposure(1000.0f) == 64.0f);
    CHECK(sanitizePostProcessExposure(
              std::numeric_limits<float>::quiet_NaN()) == 1.0f);
    CHECK(sanitizePostProcessExposure(
              std::numeric_limits<float>::infinity()) == 1.0f);
}

TEST_CASE(r51_gamma_sanitizer_rejects_nonpositive_and_nonfinite)
{
    using ayt::render::detail::sanitizePostProcessGamma;
    CHECK(sanitizePostProcessGamma(2.2f) == 2.2f);
    CHECK(sanitizePostProcessGamma(0.01f) == 0.1f);
    CHECK(sanitizePostProcessGamma(100.0f) == 8.0f);
    CHECK(sanitizePostProcessGamma(0.0f) == 2.2f);
    CHECK(sanitizePostProcessGamma(-1.0f) == 2.2f);
    CHECK(sanitizePostProcessGamma(
              std::numeric_limits<float>::quiet_NaN()) == 2.2f);
}

TEST_CASE(r51_primary_source_is_the_live_runtime_source)
{
    const std::string source(
        ayt::render::detail::postProcessPhoskiaSourceForTests());
    CHECK(source.find("material PostProcess") != std::string::npos);
    CHECK(source.find("texture2d sceneColor") != std::string::npos);
    CHECK(source.find("texture2d bloomTexture") != std::string::npos);
    CHECK(source.find("uniform vec4 exposure") != std::string::npos);
    CHECK(source.find("uniform vec4 gammaParams") != std::string::npos);
    CHECK(source.find("bloomSample.xyz * bloomStrength.x * exposure.x")
          != std::string::npos);
    CHECK(source.find("step(1.5, m)") != std::string::npos);
    CHECK(source.find("uTime") == std::string::npos);
}

TEST_CASE(r51_fallback_is_an_independent_minimal_blit)
{
    const std::string source(
        ayt::render::detail::postProcessFallbackPhoskiaSourceForTests());
    CHECK(source.find("material PostProcessBlit") != std::string::npos);
    CHECK(source.find("texture2d sceneColor") != std::string::npos);
    CHECK(source.find("return sample(sceneColor, uv)") != std::string::npos);
    CHECK(source.find("bloomTexture") == std::string::npos);
    CHECK(source.find("tonemapMode") == std::string::npos);
    CHECK(source.find("gammaParams") == std::string::npos);
    CHECK(source.find("pow(") == std::string::npos);
}

TEST_CASE(r51_cache_keys_pin_both_production_programs)
{
    CHECK(std::string(ayt::render::detail::kPostProcessCacheKeyCStr)
          == "postprocess_tonemap_aces_v12_sanitized_params_fs");
    CHECK(std::string(ayt::render::detail::kPostProcessFallbackCacheKeyCStr)
          == "postprocess_fallback_blit_v11_minimal_fs");
}

TEST_CASE(r51_production_sources_compile_and_expose_expected_bindings)
{
    ayt::shader::CompiledShaderProgram primary;
    compileProductionSource(
        ayt::render::detail::postProcessPhoskiaSourceForTests(), primary);
    if (!primary.success) {
        printCompileErrors(primary, "primary");
    }
    CHECK(primary.success);
    if (primary.success) {
        CHECK(hasUniform(primary, "bloomStrength"));
        CHECK(hasUniform(primary, "exposure"));
        CHECK(hasUniform(primary, "tonemapMode"));
        CHECK(hasUniform(primary, "gammaParams"));
        CHECK_FALSE(hasUniform(primary, "uTime"));
        CHECK(hasTexture(primary, "sceneColor"));
        CHECK(hasTexture(primary, "bloomTexture"));
    }

    ayt::shader::CompiledShaderProgram fallback;
    compileProductionSource(
        ayt::render::detail::postProcessFallbackPhoskiaSourceForTests(),
        fallback);
    if (!fallback.success) {
        printCompileErrors(fallback, "fallback");
    }
    CHECK(fallback.success);
    if (fallback.success) {
        CHECK(hasTexture(fallback, "sceneColor"));
        CHECK_FALSE(hasTexture(fallback, "bloomTexture"));
    }
}

TEST_CASE(r51_public_setters_frame_bridge_and_lifecycle_are_hardened)
{
    const std::string source = readRendererFile("src/AYRenderer.cpp");
    CHECK(!source.empty());
    CHECK(source.find("sanitizePostProcessExposure(exposure)")
          != std::string::npos);
    CHECK(source.find("sanitizePostProcessGamma(gamma)")
          != std::string::npos);

    const std::size_t shutdown = source.find("void Renderer::shutdown()");
    const std::size_t post = source.find(
        "_impl->pipeline.findPass(\"PostProcess\")", shutdown);
    const std::size_t shaderPool = source.find(
        "_impl->shaderPool.shutdown()", shutdown);
    const std::size_t adapter = source.find("_impl->adapter.shutdown()", shutdown);
    CHECK(post != std::string::npos);
    CHECK(shaderPool != std::string::npos);
    CHECK(adapter != std::string::npos);
    CHECK(post < shaderPool);
    CHECK(post < adapter);
}

TEST_CASE(r51_forward_clear_is_touched_before_draw_iteration)
{
    const std::string source = readRendererFile(
        "src/detail/ForwardOpaquePass.cpp");
    const std::size_t clear = source.find("adapter.setViewClearRaw(");
    const std::size_t touch = source.find("adapter.touch(viewId);", clear);
    const std::size_t loop = source.find(
        "for (const DrawItem& item : scene.items())", touch);
    CHECK(clear != std::string::npos);
    CHECK(touch != std::string::npos);
    CHECK(loop != std::string::npos);
    CHECK(clear < touch);
    CHECK(touch < loop);
}

TEST_CASE(r51_scene_color_routing_is_shared_by_all_consumers)
{
    const std::string bloom = readRendererFile(
        "src/detail/BloomExtractPass.cpp");
    const std::string haze = readRendererFile(
        "src/detail/DepthHazePass.cpp");
    const std::string transparent = readRendererFile(
        "src/detail/TransparentPass.cpp");
    const std::string sources[] = {bloom, haze, transparent};
    for (const std::string& source : sources) {
        CHECK(source.find("selectSceneColorSourceFbo(ctx)")
              != std::string::npos);
        CHECK(source.find("PostProcessPass::selectSourceFbo(ctx)")
              == std::string::npos);
    }
}

TEST_CASE(r51_program_failure_uses_bounded_retry)
{
    const std::string source = readRendererFile(
        "src/detail/PostProcessPass.cpp");
    CHECK(source.find("kRetryIntervalFrames = 120") != std::string::npos);
    CHECK(source.find("--_programRetryFrames") != std::string::npos);
    CHECK(source.find("ProgramVariant::Fallback") != std::string::npos);
    CHECK(source.find("_programAcquireFailed") == std::string::npos);
}

TEST_SUITE_END
