#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/ColorStylizationShaderSources.h"
#include "AYShader/Phoskia.h"
#include "AYShader/BGFXConverter.h"
#include "detail/ColorStylizationPass.h"
#include "detail/FgResource.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/RenderPassContract.h"
#include "detail/RenderViewNamespaces.h"

#include <algorithm>
#include <iostream>
#include <limits>

using namespace ayt::render;
using namespace ayt::render::detail;

TEST_SUITE(AYRenderer_ColorStylization)

TEST_CASE(settings_presets_identity_and_sanitization)
{
    Renderer renderer;
    CHECK_FALSE(colorStylizationActive(renderer.colorStylizationSettings()));
    for (auto preset : {ColorStylizationPreset::PixelRetro,
                       ColorStylizationPreset::LowColor,
                       ColorStylizationPreset::DitherRetro}) {
        renderer.setColorStylizationSettings(colorStylizationPreset(preset));
        renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
        CHECK(colorStylizationActive(renderer.colorStylizationSettings()));
        renderer.configurePipeline(RenderPipelineDesc::makeDefault());
        CHECK(colorStylizationActive(renderer.colorStylizationSettings()));
    }
    auto settings = colorStylizationPreset(ColorStylizationPreset::Neutral);
    CHECK_FALSE(colorStylizationActive(settings));
    settings.enabled=true; settings.posterizationEnabled=false; settings.pixelSize=1;
    settings.ditherStrength=1;
    CHECK_FALSE(colorStylizationActive(settings)); // Dither alone has no quantizer.
    settings.pixelSize=0; settings.colorLevels=255;
    settings.ditherStrength=std::numeric_limits<float>::quiet_NaN();
    renderer.setColorStylizationSettings(settings);
    settings=renderer.colorStylizationSettings();
    CHECK(settings.pixelSize==1); CHECK(settings.colorLevels==32);
    CHECK(settings.ditherStrength==0);
    settings.pixelSize=65535; settings.colorLevels=0; settings.ditherStrength=5;
    settings=sanitizeColorStylization(settings);
    CHECK(settings.pixelSize==64); CHECK(settings.colorLevels==2);
    CHECK(settings.ditherStrength==1);
}

TEST_CASE(canonical_order_and_reserved_view)
{
    CHECK(static_cast<uint8_t>(RenderPassSlot::ColorStylization)==25);
    CHECK(static_cast<uint8_t>(FgResourceId::StylizedColor)==30);
    for (auto desc : {RenderPipelineDesc::makeDefault(),RenderPipelineDesc::makeDeferred(),
                      RenderPipelineDesc::makeEditorForward(),RenderPipelineDesc::makeEditorDeferred()}) {
        auto style=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::ColorStylization);
        CHECK(style!=desc.passes.end());
        CHECK(*(style-1)==RenderPassSlot::ColorGrading);
        CHECK(*(style+1)==RenderPassSlot::Present);
        CHECK(validateRenderPipelineContracts(desc.passes).valid());
    }
    auto view=std::find(kRenderViewOrder.begin(),kRenderViewOrder.end(),ColorStylizationPass::kViewId);
    CHECK(view!=kRenderViewOrder.end()); CHECK(*(view-1)==4); CHECK(*(view+1)==16);
    CHECK(nextUiViewRange(50,1)==51);
    Renderer renderer;
    renderer.configurePipeline({{RenderPassSlot::PostProcess,RenderPassSlot::ColorStylization,RenderPassSlot::UI}});
    CHECK(renderer.pipelineDesc().passes[2]==RenderPassSlot::Present);
    const std::array wrong={RenderPassSlot::PostProcess,RenderPassSlot::Present,RenderPassSlot::ColorStylization};
    CHECK_FALSE(validateRenderPipelineContracts(wrong).valid());
}

TEST_CASE(graph_splices_after_each_aa_and_grading_and_has_no_disabled_target)
{
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    for (int aa=0;aa<4;++aa) for (bool grading : {false,true}) {
        PostProcessGraphPlanInput input{};
        input.width=319;input.height=181;input.sceneColor={17};
        input.finalLdr=true;input.fxaa=aa==1;input.smaa=aa==2;
        input.taa=aa==3;input.taaWriteTarget={18};input.taaReadHistory={19};
        input.gbuffer={20}; input.motionVectors={21}; input.colorGrading=grading;
        auto off=buildPostProcessGraphPlan(graph,input);
        CHECK(off.compileSucceeded);
        const auto offPasses=graph.stats().declaredPasses;
        const auto offTargets=graph.stats().physicalTargets;
        CHECK_FALSE(graph.shouldExecute(RenderPassSlot::ColorStylization));
        input.colorStylization=true;
        auto on=buildPostProcessGraphPlan(graph,input);
        CHECK(on.compileSucceeded);
        CHECK(on.presentationSource==FgResourceId::StylizedColor);
        CHECK(graph.shouldExecute(RenderPassSlot::ColorStylization));
        CHECK(graph.stats().declaredPasses==offPasses+1);
        // The conservative graph keeps each live output independent.
        CHECK(graph.stats().physicalTargets==offTargets+1);
        CHECK_FALSE(graph.producedThisFrame(FgResourceId::StylizedColor));
        CHECK_FALSE(graph.semanticProducedThisFrame(FgSemantic::PresentSource));
    }
}

TEST_CASE(uninitialized_pass_does_not_promote_or_acquire_resources)
{
    BGFXAdapter adapter; ayt::shader::ShaderResourcePool pool;
    RenderScene scene; FrameContext frame;
    std::unordered_map<uint64_t,GpuMesh> meshes;
    std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    PassExecContext context{adapter,pool,scene,meshes,textures,materials,0,0,320,180,frame,0};
    ColorStylizationPass pass;
    pass.setSettings(colorStylizationPreset(ColorStylizationPreset::DitherRetro));
    CHECK(pass.execute(context)==0);
    pass.destroyResources(adapter); pass.destroyResources(adapter);
}

TEST_CASE(shader_compiles_portable_and_d3d_with_complete_bindings)
{
    for (const char* profile : {"430","s_5_0"}) {
#ifndef _WIN32
        if (std::string(profile)=="s_5_0") continue;
#endif
        ayt::shader::phoskia::Compiler compiler;
        ayt::shader::BGFXCompileOptions backend;
        backend.shadercPath=AY_SHADER_SHADERC_HINT;
        backend.platform=std::string(profile)=="430" ? "linux" : "windows";
        backend.profile=profile;
#ifdef AY_SHADER_BGFX_COMMON_HINT
        backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
        backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
        ayt::shader::CompiledShaderProgram program;
        compiler.compileToProgram(kColorStylizationPhoskiaSource,{},backend,program);
        for (auto& error : program.errors) std::cerr<<error<<'\n';
        CHECK(program.success);
        CHECK(std::any_of(program.textures.begin(),program.textures.end(),[](auto& t){return t.name=="inputColor";}));
        CHECK(std::any_of(program.uniforms.begin(),program.uniforms.end(),[](auto& u){return u.name=="styleParams";}));
        CHECK(std::any_of(program.uniforms.begin(),program.uniforms.end(),[](auto& u){return u.name=="styleViewport";}));
    }
}
TEST_SUITE_END
