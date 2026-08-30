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
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/PresentPass.h"
#include "detail/RenderViewOrder.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
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

bool hasTexture(const ayt::shader::CompiledShaderProgram& program,
                const char* name)
{
    return std::any_of(program.textures.begin(), program.textures.end(),
        [name](const ayt::shader::BGFXTexture& texture) {
            return texture.name == name;
        });
}

bool hasUniform(const ayt::shader::CompiledShaderProgram& program,
                const char* name)
{
    return std::any_of(program.uniforms.begin(), program.uniforms.end(),
        [name](const ayt::shader::BGFXUniform& uniform) {
            return uniform.name == name;
        });
}

} // namespace

TEST_SUITE(AYRenderer_FXAA)

TEST_CASE(fxaa_append_only_abi_and_view_are_locked)
{
    CHECK(static_cast<uint8_t>(RenderPassSlot::FXAA) == 16u);
    CHECK(static_cast<uint8_t>(FgResourceId::FxaaColor) == 7u);
    CHECK(static_cast<uint8_t>(FgResourceId::Count) == 9u);
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
        const std::size_t grading = passIndex(desc, RenderPassSlot::ColorGrading);
        const std::size_t present = passIndex(desc, RenderPassSlot::Present);
        CHECK(fxaa == post + 1u);
        CHECK(grading == fxaa + 1u);
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
    const auto present = std::find(order.begin(), order.end(),
                                   ayt::render::detail::PresentPass::kPresentViewId);
    CHECK(pp != order.end());
    CHECK(fxaa == pp + 1);
    CHECK(grading == fxaa + 1);
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

TEST_CASE(fxaa_shader_compiles_reflects_bindings_and_clamps_edges)
{
    const std::string source =
        ayt::render::detail::fxaaPhoskiaSourceForTests();
    CHECK(source.find("clamp(uv +") != std::string::npos);
    CHECK(source.find("dot(rgbNW, luma)") != std::string::npos);
    CHECK(source.find("mix(rgbA, rgbB, useB)") != std::string::npos);

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
            std::cerr << "[FXAAPass test] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(hasTexture(program, "inputColor"));
    CHECK(hasUniform(program, "inverseViewport"));
    CHECK(std::string(ayt::render::detail::kFxaaCacheKeyCStr)
          == "fxaa_311_luma_v1");
}

TEST_SUITE_END
