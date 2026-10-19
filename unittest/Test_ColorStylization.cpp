#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/ColorStylizationShaderSources.h"
#include "AYRenderer/ColorStylizationLook.h"
#include "AYShader/Phoskia.h"
#include "AYShader/BGFXConverter.h"
#include "detail/ColorStylizationPass.h"
#include "detail/StylizedOutlinePass.h"
#include "AYRenderer/StylizedOutlineShaderSources.h"
#include "AYRenderer/AnalogDisplayShaderSources.h"
#include "AYRenderer/TemporalFeedbackShaderSources.h"
#include "detail/TemporalFeedbackPass.h"
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
        CHECK(*(style+1)==RenderPassSlot::TemporalFeedback);
        CHECK(*(style+2)==RenderPassSlot::AnalogDisplay);
        CHECK(*(style+3)==RenderPassSlot::Present);
        CHECK(validateRenderPipelineContracts(desc.passes).valid());
    }
    auto view=std::find(kRenderViewOrder.begin(),kRenderViewOrder.end(),ColorStylizationPass::kViewId);
    CHECK(view!=kRenderViewOrder.end()); CHECK(*(view-1)==4); CHECK(*(view+1)==53);
    CHECK(nextUiViewRange(50,1)==54);
    CHECK(static_cast<uint8_t>(RenderPassSlot::StylizedOutline)==26);
    Renderer renderer;
    renderer.configurePipeline({{RenderPassSlot::PostProcess,RenderPassSlot::ColorStylization,RenderPassSlot::UI}});
    CHECK(renderer.pipelineDesc().passes[2]==RenderPassSlot::Present);
    const std::array wrong={RenderPassSlot::PostProcess,RenderPassSlot::Present,RenderPassSlot::ColorStylization};
    CHECK_FALSE(validateRenderPipelineContracts(wrong).valid());
}

TEST_CASE(palette_duotone_and_print_are_independent_and_bounded)
{
    ColorStylizationSettings s;
    s.enabled=true;s.pixelationEnabled=false;s.posterizationEnabled=false;
    CHECK_FALSE(colorStylizationActive(s));
    s.colorMapping=StylizationColorMapping::FixedPalette;
    CHECK(colorStylizationActive(s));
    s.paletteSize=255;s.palette[15]={-1,std::numeric_limits<float>::infinity(),2};
    s.printSpacing=std::numeric_limits<float>::quiet_NaN();s.printAngle=-405;
    s.printStrength=2;s=sanitizeColorStylization(s);
    CHECK(s.paletteSize==16);CHECK(s.palette[15].r==0);
    CHECK(s.palette[15].g==0);CHECK(s.palette[15].b==1);
    CHECK(s.printSpacing==7);CHECK(s.printAngle==135);CHECK(s.printStrength==1);
    s.paletteSize=0;s.colorMapping=static_cast<StylizationColorMapping>(255);
    s.printPattern=static_cast<StylizationPrintPattern>(255);
    s=sanitizeColorStylization(s);
    CHECK(s.paletteSize==2);CHECK_FALSE(colorStylizationActive(s));
    s.printPattern=StylizationPrintPattern::Dots;CHECK(colorStylizationActive(s));
    s.printStrength=0;CHECK_FALSE(colorStylizationActive(s));
    for(auto preset:{ColorStylizationPreset::Pocket,ColorStylizationPreset::Sunset,
        ColorStylizationPreset::Noir,ColorStylizationPreset::Comic,ColorStylizationPreset::Engraving}) {
        auto look=sanitizeColorStylization(colorStylizationPreset(preset));
        CHECK(colorStylizationActive(look));CHECK(look.paletteSize<=16);
    }
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
        ayt::shader::CompiledShaderProgram outline;
        compiler.compileToProgram(kStylizedOutlinePhoskiaSource,{},backend,outline);
        for(auto& error:outline.errors)std::cerr<<error<<'\n';
        CHECK(outline.success);
        CHECK(std::any_of(outline.textures.begin(),outline.textures.end(),[](auto& t){return t.name=="outlineDepth";}));
        CHECK(std::any_of(outline.uniforms.begin(),outline.uniforms.end(),[](auto& u){return u.name=="outlineInverseProjection";}));
        ayt::shader::CompiledShaderProgram display;
        compiler.compileToProgram(analogDisplayPhoskiaSource(),{},backend,display);
        for(auto& error:display.errors)std::cerr<<error<<'\n';
        CHECK(display.success);
        CHECK(std::any_of(display.uniforms.begin(),display.uniforms.end(),[](auto& u){return u.name=="displayViewport";}));
        ayt::shader::CompiledShaderProgram feedback;
        compiler.compileToProgram(temporalFeedbackPhoskiaSource(),{},backend,feedback);
        for(auto& error:feedback.errors)std::cerr<<error<<'\n';
        CHECK(feedback.success);
        CHECK(std::any_of(feedback.textures.begin(),feedback.textures.end(),[](auto& t){return t.name=="previousColor";}));
    }
}

TEST_CASE(look_document_round_trip_rejects_unknown_and_preserves_output)
{
    auto original=colorStylizationPreset(ColorStylizationPreset::Blueprint);
    original.palette[15]={.25f,.5f,.75f};original.outlineWidth=3;
    const auto text=serializeColorStylizationLook(original);
    ColorStylizationSettings output;
    std::string error;
    CHECK(parseColorStylizationLook(text,output,&error));CHECK(error.empty());
    CHECK(serializeColorStylizationLook(output)==text);
    for(const auto& bad:{std::string{},std::string("{}"),std::string(65537,' '),
        std::string("{\"schemaVersion\":1,\"schemaVersion\":1}"),
        text.substr(0,text.size()/2)}) {
        CHECK_FALSE(parseColorStylizationLook(bad,output,&error));CHECK_FALSE(error.empty());
        CHECK(serializeColorStylizationLook(output)==text);
    }
    for(const auto& pair:std::array<std::pair<std::string,std::string>,5>{{
        {"\"schemaVersion\": 3","\"schemaVersion\": 4"},
        {"\"outlineWidth\": 3.0","\"outlineWidth\": 99"},
        {"\"pixelSize\": 6","\"pixelSize\": 1.5"},
        {"\"enabled\": true","\"enabled\": 1"},
        {"\"colorMapping\": 1","\"colorMapping\": 255"}}}) {
        auto invalid=text;const auto pos=invalid.find(pair.first);CHECK(pos!=std::string::npos);
        if(pos==std::string::npos)continue;
        invalid.replace(pos,pair.first.size(),pair.second);
        CHECK_FALSE(parseColorStylizationLook(invalid,output,&error));
        CHECK(serializeColorStylizationLook(output)==text);
    }
    auto extra=text;extra.insert(extra.find('{')+1,"\"typo\":true,");
    CHECK_FALSE(parseColorStylizationLook(extra,output,&error));
    auto duplicate=text;duplicate.insert(duplicate.find('{')+1,"\"enabled\":true,");
    CHECK_FALSE(parseColorStylizationLook(duplicate,output,&error));
}

TEST_CASE(outline_has_independent_gate_and_graph_dependency)
{
    ColorStylizationSettings s;s.enabled=true;s.pixelationEnabled=false;s.posterizationEnabled=false;
    s.outlineMode=StylizationOutlineMode::DepthNormal;
    CHECK_FALSE(colorStylizationActive(s));CHECK(stylizationOutlineActive(s));
    s.outlineStrength=0;CHECK_FALSE(stylizationOutlineActive(s));
    BGFXAdapter adapter;FrameGraph graph(adapter);
    PostProcessGraphPlanInput in;in.width=320;in.height=180;in.sceneColor={17};in.gbuffer={18};
    in.finalLdr=true;in.stylizedOutline=true;in.stylizationDepth=true;in.stylizationNormals=true;
    auto plan=buildPostProcessGraphPlan(graph,in);CHECK(plan.compileSucceeded);
    CHECK(plan.presentationSource==FgResourceId::OutlinedColor);
    CHECK(graph.shouldExecute(RenderPassSlot::StylizedOutline));
    CHECK_FALSE(graph.shouldExecute(RenderPassSlot::ColorStylization));
    in.colorGrading=true;in.colorStylization=true;
    CHECK(buildPostProcessGraphPlan(graph,in).compileSucceeded);
    CHECK(graph.shouldExecute(RenderPassSlot::StylizedOutline));
    CHECK(graph.shouldExecute(RenderPassSlot::ColorGrading));
    CHECK(graph.shouldExecute(RenderPassSlot::ColorStylization));
    in.stylizedOutline=false;in.stylizationDepth=false;in.stylizationNormals=false;
    CHECK(buildPostProcessGraphPlan(graph,in).compileSucceeded);
    CHECK_FALSE(graph.shouldExecute(RenderPassSlot::StylizedOutline));
}

TEST_CASE(display_bypasses_identity_and_migrates_v1_look_with_display_off)
{
    AnalogDisplaySettings s;CHECK_FALSE(analogDisplayActive(s));
    s.enabled=true;s.scanlineStrength=s.maskStrength=s.chromaticOffset=s.curvature=s.vignetteStrength=s.grainStrength=0;
    CHECK_FALSE(analogDisplayActive(s));s.scanlineStrength=std::numeric_limits<float>::infinity();
    s.maskStrength=-1;s.curvature=99;s.grainStrength=99;s=sanitizeAnalogDisplay(s);
    CHECK(s.scanlineStrength==0);CHECK(s.maskStrength==0);CHECK(s.curvature==.25f);CHECK(s.grainStrength==.25f);
    CHECK(analogDisplayActive(s));
    ColorStylizationSettings old;std::string error;
    CHECK(loadColorStylizationLook(std::string(AY_RENDERER_SOURCE_DIR)+"/demo/looks/9.rsl",old,&error));
    CHECK_FALSE(old.display.enabled);CHECK(old.outlineMode==StylizationOutlineMode::Depth);
    auto modern=colorStylizationPreset(ColorStylizationPreset::Arcade);
    CHECK_FALSE(colorStylizationActive(modern));CHECK(analogDisplayActive(modern.display));
    const auto text=serializeColorStylizationLook(modern);ColorStylizationSettings restored;
    CHECK(parseColorStylizationLook(text,restored,&error));
    CHECK(serializeColorStylizationLook(restored)==text);
    BGFXAdapter a;FrameGraph graph(a);PostProcessGraphPlanInput in;
    in.width=320;in.height=180;in.sceneColor={1};in.finalLdr=true;in.analogDisplay=true;
    const auto plan=buildPostProcessGraphPlan(graph,in);CHECK(plan.compileSucceeded);
    CHECK(plan.presentationSource==FgResourceId::DisplayColor);
    CHECK_FALSE(graph.shouldExecute(RenderPassSlot::ColorStylization));CHECK(graph.shouldExecute(RenderPassSlot::AnalogDisplay));
}

TEST_CASE(temporal_settings_identity_schema_migration_and_no_device_gate)
{
    TemporalFeedbackSettings s;CHECK_FALSE(temporalFeedbackActive(s));
    s.enabled=true;s.mode=TemporalFeedbackMode::FrameHold;s.holdFrames=1;CHECK_FALSE(temporalFeedbackActive(s));
    s.holdFrames=0;s.tileSize=0;s.historyWeight=std::numeric_limits<float>::quiet_NaN();
    s.historyOffsetX=std::numeric_limits<float>::infinity();s=sanitizeTemporalFeedback(s);
    CHECK(s.holdFrames==1);CHECK(s.tileSize==4);CHECK(s.historyWeight==0);CHECK(s.historyOffsetX==0);
    s.holdFrames=65535;s.tileSize=65535;s.historyWeight=2;s.historyOffsetX=99;s.historyOffsetY=-99;
    s=sanitizeTemporalFeedback(s);CHECK(s.holdFrames==120);CHECK(s.tileSize==128);
    CHECK(s.historyWeight==1);CHECK(s.historyOffsetX==64);CHECK(s.historyOffsetY==-64);
    ColorStylizationSettings legacy;std::string error;
    CHECK(loadColorStylizationLook(std::string(AY_RENDERER_SOURCE_DIR)+"/demo/looks/arcade.rsl",legacy,&error));
    CHECK(legacy.display.enabled);CHECK_FALSE(legacy.temporal.enabled);
    auto look=colorStylizationPreset(ColorStylizationPreset::TimeEcho);
    auto text=serializeColorStylizationLook(look);ColorStylizationSettings restored;
    CHECK(parseColorStylizationLook(text,restored,&error));CHECK(serializeColorStylizationLook(restored)==text);
    TemporalFeedbackPass pass;pass.setSettings(look);BGFXAdapter a;ayt::shader::ShaderResourcePool pool;FrameContext frame;
    CHECK_FALSE(pass.prepareFrame(a,pool,frame,0,0,320,180));
    CHECK_FALSE(BGFXAdapter::isValid(pass.readFbo()));CHECK_FALSE(BGFXAdapter::isValid(pass.writeFbo()));
    CHECK(pass.generation()==0);CHECK(pass.completedFrames()==0);CHECK(pool.cacheStats().sourceMisses==0);
    pass.destroyResources(a);pass.destroyResources(a);
    FrameGraph graph(a);PostProcessGraphPlanInput in;in.width=320;in.height=180;
    in.sceneColor={1};in.finalLdr=true;in.temporalFeedback=true;in.feedbackReadHistory={2};in.feedbackWriteTarget={3};
    const auto plan=buildPostProcessGraphPlan(graph,in);CHECK(plan.compileSucceeded);
    CHECK(plan.presentationSource==FgResourceId::TemporalColor);CHECK(graph.shouldExecute(RenderPassSlot::TemporalFeedback));
    CHECK_FALSE(graph.shouldExecute(RenderPassSlot::AnalogDisplay));
}
TEST_SUITE_END
