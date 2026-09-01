#include "AYTest.h"

#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "AYShader/ShaderResourcePool.h"

#include "detail/BGFXAdapter.h"
#include "detail/ColorGradingPass.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/FXAAPass.h"
#include "detail/SMAAPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/PresentPass.h"
#include "detail/RenderViewOrder.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

using ayt::render::ColorGradingPreset;
using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgSemantic;

std::size_t gradingPassIndex(const RenderPipelineDesc& desc,
                             RenderPassSlot slot)
{
    const auto it = std::find(desc.passes.begin(), desc.passes.end(), slot);
    return it == desc.passes.end()
        ? desc.passes.size()
        : static_cast<std::size_t>(it - desc.passes.begin());
}

bool gradingHasTexture(const ayt::shader::CompiledShaderProgram& program,
                       const char* name)
{
    return std::any_of(program.textures.begin(), program.textures.end(),
        [name](const ayt::shader::BGFXTexture& texture) {
            return texture.name == name;
        });
}

bool gradingHasUniform(const ayt::shader::CompiledShaderProgram& program,
                       const char* name)
{
    return std::any_of(program.uniforms.begin(), program.uniforms.end(),
        [name](const ayt::shader::BGFXUniform& uniform) {
            return uniform.name == name;
        });
}

} // namespace

TEST_SUITE(AYRenderer_ColorGrading)

TEST_CASE(color_grading_append_only_abi_and_view_are_locked)
{
    CHECK(static_cast<uint8_t>(RenderPassSlot::ColorGrading) == 17u);
    CHECK(static_cast<uint8_t>(FgResourceId::ColorGradedColor) == 8u);
    CHECK(static_cast<uint8_t>(FgResourceId::Count) == 12u);
    CHECK(ayt::render::detail::ColorGradingPass::kColorGradingViewId == 4u);
    CHECK(std::is_final_v<ayt::render::detail::ColorGradingPass>);
}

TEST_CASE(product_pipelines_order_grading_between_smaa_and_present)
{
    for (const RenderPipelineDesc desc : {
             RenderPipelineDesc::makeDefault(),
             RenderPipelineDesc::makeDeferred(),
             RenderPipelineDesc::makeEditorForward(),
             RenderPipelineDesc::makeEditorDeferred()}) {
        const std::size_t fxaa = gradingPassIndex(desc, RenderPassSlot::FXAA);
        const std::size_t smaa = gradingPassIndex(desc, RenderPassSlot::SMAA);
        const std::size_t grading =
            gradingPassIndex(desc, RenderPassSlot::ColorGrading);
        const std::size_t present =
            gradingPassIndex(desc, RenderPassSlot::Present);
        CHECK(smaa == fxaa + 1u);
        CHECK(grading == smaa + 1u);
        CHECK(present == grading + 1u);
    }
}

TEST_CASE(color_grading_view_executes_after_smaa_and_before_present)
{
    const auto& order = ayt::render::detail::kRenderViewOrder;
    const auto fxaa = std::find(order.begin(), order.end(),
                                ayt::render::detail::FXAAPass::kFxaaViewId);
    const auto smaa = std::find(
        order.begin(), order.end(),
        ayt::render::detail::SMAAPass::kNeighborhoodViewId);
    const auto grading = std::find(
        order.begin(), order.end(),
        ayt::render::detail::ColorGradingPass::kColorGradingViewId);
    const auto present = std::find(
        order.begin(), order.end(),
        ayt::render::detail::PresentPass::kPresentViewId);
    CHECK(fxaa != order.end());
    CHECK(smaa == fxaa + 3);
    CHECK(grading == smaa + 1);
    CHECK(present == grading + 1);
}

TEST_CASE(color_grading_defaults_off_and_knobs_survive_pipeline_rebuild)
{
    ayt::render::Renderer renderer;
    CHECK_FALSE(renderer.colorGradingEnabled());
    CHECK_FLOAT_EQ(renderer.colorGradingStrength(), 0.75f, 1.0e-6f);
    CHECK(renderer.colorGradingPreset() == ColorGradingPreset::Warm);

    renderer.setColorGradingEnabled(true);
    renderer.setColorGradingStrength(0.4f);
    renderer.setColorGradingPreset(ColorGradingPreset::Cinematic);
    renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
    CHECK(renderer.colorGradingEnabled());
    CHECK_FLOAT_EQ(renderer.colorGradingStrength(), 0.4f, 1.0e-6f);
    CHECK(renderer.colorGradingPreset() == ColorGradingPreset::Cinematic);
}

TEST_CASE(color_grading_public_knobs_sanitize_invalid_values)
{
    ayt::render::Renderer renderer;
    renderer.setColorGradingStrength(-1.0f);
    CHECK_FLOAT_EQ(renderer.colorGradingStrength(), 0.0f, 1.0e-6f);
    renderer.setColorGradingStrength(2.0f);
    CHECK_FLOAT_EQ(renderer.colorGradingStrength(), 1.0f, 1.0e-6f);
    renderer.setColorGradingStrength(
        std::numeric_limits<float>::quiet_NaN());
    CHECK_FLOAT_EQ(renderer.colorGradingStrength(), 0.0f, 1.0e-6f);
    renderer.setColorGradingPreset(static_cast<ColorGradingPreset>(255u));
    CHECK(renderer.colorGradingPreset() == ColorGradingPreset::Neutral);
}

TEST_CASE(old_custom_descriptor_inserts_present_after_explicit_grading)
{
    ayt::render::Renderer renderer;
    RenderPipelineDesc custom{{RenderPassSlot::PostProcess,
                               RenderPassSlot::FXAA,
                               RenderPassSlot::ColorGrading,
                               RenderPassSlot::UI}};
    renderer.configurePipeline(custom);
    const RenderPipelineDesc& resolved = renderer.pipelineDesc();
    CHECK(resolved.passes.size() == 5u);
    CHECK(resolved.passes[0] == RenderPassSlot::PostProcess);
    CHECK(resolved.passes[1] == RenderPassSlot::FXAA);
    CHECK(resolved.passes[2] == RenderPassSlot::ColorGrading);
    CHECK(resolved.passes[3] == RenderPassSlot::Present);
    CHECK(resolved.passes[4] == RenderPassSlot::UI);
}

TEST_CASE(present_source_promotes_only_after_grading_production)
{
    ayt::render::detail::BGFXAdapter adapter;
    ayt::render::detail::FrameGraph fg(adapter);
    fg.beginFrame(640, 360);
    fg.addResource(FgResourceId::FinalLdrColor,
                   {bgfx::TextureFormat::RGBA8,
                    ayt::render::detail::FgTextureScale::Full,
                    true,
                    false});
    fg.addResource(FgResourceId::ColorGradedColor,
                   {bgfx::TextureFormat::RGBA8,
                    ayt::render::detail::FgTextureScale::Full,
                    true,
                    false});
    fg.addPass({"PostProcess", {}, {FgResourceId::FinalLdrColor}, true});
    fg.addPass({"ColorGrading",
                {FgResourceId::FinalLdrColor},
                {FgResourceId::ColorGradedColor},
                true});
    fg.setResolvedSemantic(FgSemantic::PresentSource,
                           FgResourceId::FinalLdrColor);
    CHECK(fg.compile());
    fg.markProduced(FgResourceId::FinalLdrColor);
    CHECK(fg.semanticProducedThisFrame(FgSemantic::PresentSource));
    CHECK_FALSE(fg.producedThisFrame(FgResourceId::ColorGradedColor));

    fg.markProduced(FgResourceId::ColorGradedColor);
    fg.setResolvedSemantic(FgSemantic::PresentSource,
                           FgResourceId::ColorGradedColor);
    CHECK(fg.semanticProducedThisFrame(FgSemantic::PresentSource));
}

TEST_CASE(procedural_luts_have_expected_layout_and_non_neutral_presets)
{
    constexpr std::size_t size =
        ayt::render::detail::ColorGradingPass::kLutSize;
    const auto neutral = ayt::render::detail::generateColorGradingLutRgba8(
        ColorGradingPreset::Neutral);
    const auto warm = ayt::render::detail::generateColorGradingLutRgba8(
        ColorGradingPreset::Warm);
    CHECK(neutral.size() == size * size * size * 4u);
    CHECK(warm.size() == neutral.size());
    CHECK(neutral.front() == 0u);
    CHECK(neutral[3] == 255u);
    CHECK(neutral != warm);
}

TEST_CASE(color_grading_noop_backend_returns_zero)
{
    ayt::render::detail::ColorGradingPass pass;
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

TEST_CASE(color_grading_shader_compiles_and_reflects_lut_bindings)
{
    const std::string source =
        ayt::render::detail::colorGradingPhoskiaSourceForTests();
    CHECK(source.find("blue0 = floor(scaledZ)") != std::string::npos);
    CHECK(source.find("clamp(source.xyz") == std::string::npos);
    CHECK(source.find("mix(graded0, graded1, blueMix)") != std::string::npos);
    CHECK(source.find("mix(source.xyz, graded, gradingParams.x)")
          != std::string::npos);

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
    ayt::shader::CompiledShaderProgram program;
    compiler.compileToProgram(source, frontend, backend, program);
    if (!program.success) {
        for (const std::string& error : program.errors) {
            std::cerr << "[ColorGradingPass test] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(gradingHasTexture(program, "inputColor"));
    CHECK(gradingHasTexture(program, "colorLut"));
    CHECK(gradingHasUniform(program, "gradingParams"));
    CHECK(std::string(ayt::render::detail::kColorGradingCacheKeyCStr)
          == "color_grading_lut2d_32_v2");
}

#ifdef _WIN32
TEST_CASE(color_grading_shader_compiles_for_d3d_s_5_0)
{
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
    backend.platform = "windows";
    backend.profile = "s_5_0";
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    ayt::shader::CompiledShaderProgram program;
    compiler.compileToProgram(
        ayt::render::detail::colorGradingPhoskiaSourceForTests(),
        frontend,
        backend,
        program);
    if (!program.success) {
        for (const std::string& error : program.errors) {
            std::cerr << "[ColorGradingPass D3D test] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(gradingHasTexture(program, "inputColor"));
    CHECK(gradingHasTexture(program, "colorLut"));
    CHECK(gradingHasUniform(program, "gradingParams"));
}
#endif

TEST_SUITE_END
