#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/PbrShaderSources.h"
#include "AYRenderer/RenderScene.h"
#include "detail/TextureImageLoader.h"
#include "detail/AnalogDisplayPass.h"
#include "detail/PresentPass.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/RenderViewOrder.h"
#include "detail/PassExecContext.h"
#include "detail/GpuResources.h"
#include "AYShader/ShaderResourcePool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace ayt::render;

TEST_SUITE(PresentationIntegrationGpu)

TEST_CASE(terminal_display_matches_separate_present_across_effects_viewports_and_runtime_gates)
{
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0])SKIP_TEST("Isolated D3D11/12 device required");
    HWND window=CreateWindowExW(0,L"STATIC",L"Presentation integration",WS_OVERLAPPED,
        0,0,96,80,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);if(!window)return;
    struct WindowCleanup{HWND h;~WindowCleanup(){DestroyWindow(h);}} cleanup{window};
    Renderer renderer;InitDesc init;init.windowHandle=window;init.width=96;init.height=80;
    init.msaa=0;init.vsync=false;
    init.backend=std::string(backend)=="d3d12"?Backend::Direct3D12:Backend::Direct3D11;
    CHECK(renderer.initialize(init));if(!renderer.isInitialized())return;
    renderer.setPostProcessGamma(1);renderer.setPostProcessExposure(1);
    renderer.setPostProcessTonemapMode(Renderer::TonemapMode::None);
    renderer.setMainCamera(ayt::math::Float4x4::identity(),ayt::math::Float4x4::identity(),{0,0,-2});
    renderer.setDirectionalLight({0,0,1},{1,1,1});
    const float vertices[][12]={
        {-.8f,-.6f,.5f,0,0,-1,1,0,0,1,0,1},{.6f,-.6f,.5f,0,0,-1,1,0,0,1,1,1},
        {.6f,.8f,.5f,0,0,-1,1,0,0,1,1,0},{-.8f,.8f,.5f,0,0,-1,1,0,0,1,0,0}};
    const uint16_t indices[]={0,2,1,0,3,2};
    VertexLayoutDesc layout;layout.add({VertexAttribute::Position,3,VertexComponentType::Float,false});
    layout.add({VertexAttribute::Normal,3,VertexComponentType::Float,false});
    layout.add({VertexAttribute::Tangent,4,VertexComponentType::Float,false});
    layout.add({VertexAttribute::TexCoord0,2,VertexComponentType::Float,false});
    auto mesh=renderer.createMesh(vertices,4,layout,indices,6);CHECK(mesh.isValid());
    std::array<uint8_t,8*8*4> texels{};
    for(int y=0;y<8;++y)for(int x=0;x<8;++x) {
        const size_t p=size_t(y*8+x)*4;
        texels[p]=uint8_t(x*31+7);texels[p+1]=uint8_t(y*29+13);
        texels[p+2]=uint8_t((x*37+y*43)%256);texels[p+3]=255;
    }
    auto texture=renderer.createTextureFromRgba8(8,8,texels.data());CHECK(texture.isValid());
    auto material=renderer.createMaterialFromPhoskia(kPbrPhoskiaSource);CHECK(material.isValid());
    renderer.setMaterialColor(material,"baseColor",1,1,1,1);
    renderer.setMaterialFloat(material,"metallic",0);renderer.setMaterialFloat(material,"roughness",.6f);
    renderer.setMaterialFloat(material,"ao",1);renderer.setMaterialFloat(material,"opacity",1);
    renderer.setMaterialFloat(material,"normalYSign",1);
    renderer.setMaterialTexture(material,"baseColorTexture",texture);
    renderer.setMaterialSurfaceProperties(material,0,.5f,true);
    RenderScene scene;scene.add(mesh,material,ayt::math::Float4x4::identity());
    const auto requested=std::getenv("AY_PRESENTATION_CAPTURE_DIRECTORY");
    const auto root=requested?std::filesystem::path(requested)/backend:
        std::filesystem::temp_directory_path()/(std::string("ay-presentation-")+backend+"-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    for(bool deferred:{false,true})for(int phase=0;phase<17;++phase) {
        std::vector<uint8_t> reference;uint16_t referenceTargets=0;
        for(bool integrate:{false,true}) {
            auto desc=deferred?RenderPipelineDesc::makeDeferred():RenderPipelineDesc::makeDefault();
            desc.integratePresentation=integrate;renderer.configurePipeline(desc);
            CHECK(renderer.pipelineDesc().integratePresentation==integrate);
            renderer.setShadowsEnabled(false);renderer.setFxaaEnabled(phase==10 || phase==15);
            renderer.setSmaaEnabled(false);renderer.setTaaEnabled(false);
            // Qualify ordinary/default 2x/4x MSAA and keep 16x as an explicit
            // unqualified fallback. Include 4x odd sub-rect, AA and history.
            renderer.setMsaaSampleCount(phase==11?16:phase==12?2:phase>=13?4:0);
            const bool subRect=phase==9 || phase==14;
            const uint16_t ox=subRect?11:0,oy=subRect?7:0;
            const uint16_t width=subRect?63:96,height=subRect?47:80;
            renderer.setViewportRect(ox,oy,width,height);
            ColorStylizationSettings look;look.enabled=true;look.pixelationEnabled=false;
            look.posterizationEnabled=false;look.display.enabled=phase!=8;
            look.display.scanlineStrength=phase==0?.8f:phase>=6?.35f:0;
            look.display.maskStrength=phase==1?.8f:phase>=6?.4f:0;
            look.display.chromaticOffset=phase==2?3:phase>=6?1.5f:0;
            look.display.curvature=phase==3?.2f:phase>=6?.08f:0;
            look.display.vignetteStrength=phase==4?.8f:phase>=6?.4f:0;
            look.display.grainStrength=phase==5?.2f:phase>=6?.07f:0;
            look.display.noiseSeed=42;
            if(phase==7 || phase==16) {
                look.pixelationEnabled=true;look.pixelSize=3;look.posterizationEnabled=true;
                look.temporal.enabled=true;look.temporal.mode=TemporalFeedbackMode::FrameHold;
                look.temporal.holdFrames=120;
            }
            renderer.setColorStylizationSettings(look);
            renderer.resetStylizationHistory();
            const auto path=root/((deferred?std::string("deferred-"):std::string("forward-"))+
                std::to_string(phase)+(integrate?"-integrated.png":"-separate.png"));
            for(int frame=0;frame<12;++frame) {
                ClearDesc clear;clear.r=frame>=4&&(phase==7 || phase==16)?.6f:.07f;clear.g=.23f;clear.b=.41f;
                renderer.beginCompositeFrame(clear,96,80);renderer.render(scene);
                if(frame==7)CHECK(renderer.captureScreenshot(path.string()));renderer.endFrame();
                CHECK(renderer.getFrameStats().graph.compileSucceeded);
                const bool direct=integrate && phase!=8 && phase!=11;
                uint32_t presents=0,displays=0;
                for(const auto& pass:renderer.getFrameStats().passes) {
                    if(pass.name=="Present")presents+=pass.drawCalls;
                    if(pass.name=="AnalogDisplay")displays+=pass.drawCalls;
                }
                CHECK(presents==(direct?0u:1u));CHECK(displays==(phase==8?0u:1u));
            }
            const auto image=detail::decodeImageFile(path.string());
            CHECK(image.width==96 && image.height==80);CHECK(image.rgba8.size()==96*80*4);
            if(!integrate) {
                reference=image.rgba8;referenceTargets=renderer.getFrameStats().graph.transientTargets;
            } else {
                const bool direct=phase!=8 && phase!=11;
                CHECK(renderer.getFrameStats().graph.transientTargets==referenceTargets-(direct?1:0));
                size_t differences=0;int largest=0;
                if(reference.size()==image.rgba8.size())for(size_t i=0;i<reference.size();++i) {
                    const auto delta=std::abs(int(reference[i])-int(image.rgba8[i]));
                    if(delta)++differences;largest=std::max(largest,delta);
                }
                std::cout<<"[Presentation A/B] backend="<<backend<<" deferred="<<deferred
                    <<" phase="<<phase<<" differentBytes="<<differences<<" maxDelta="<<largest<<'\n';
                CHECK(reference==image.rgba8);
            }
        }
    }
    renderer.destroyMaterial(material);renderer.destroyTexture(texture);renderer.destroyMesh(mesh);
    renderer.shutdown();
#else
    SKIP_TEST("Windows D3D device required");
#endif
}

TEST_CASE(failed_direct_stage_keeps_the_last_readable_color_for_present)
{
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0])SKIP_TEST("Isolated D3D11/12 device required");
    using namespace ayt::render::detail;
    HWND window=CreateWindowExW(0,L"STATIC",L"Presentation failure fallback",WS_OVERLAPPED,
        0,0,64,48,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);if(!window)return;
    struct WindowCleanup{HWND h;~WindowCleanup(){DestroyWindow(h);}} cleanup{window};
    BGFXAdapter adapter;BGFXInitParams init;init.nativeWindowHandle=window;
    init.width=64;init.height=48;init.msaa=0;init.vsync=false;
    init.backend=std::string(backend)=="d3d12"?Backend::Direct3D12:Backend::Direct3D11;
    CHECK(adapter.initialize(init));if(!adapter.isInitialized())return;
    CHECK(adapter.supportsDirectPresentation());
    adapter.setViewOrder(0,static_cast<uint16_t>(kRenderViewRemap.size()),kRenderViewRemap.data());
    ayt::shader::ShaderResourcePool pool,missingPool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT});
    pool.resolvePlatformFromRenderer();missingPool.resolvePlatformFromRenderer();
    // Deterministic production failure: no catalog entry and source compilation
    // forbidden. This pool cannot recover by finding another pool's cache.
    missingPool.setRuntimeCompilationEnabled(false);
    std::vector<uint8_t> data(31*23*4);
    for(size_t i=0;i<data.size();i+=4){data[i]=51;data[i+1]=102;data[i+2]=153;data[i+3]=255;}
    auto color=bgfx::createTexture2D(31,23,false,1,bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT|BGFX_SAMPLER_POINT,bgfx::copy(data.data(),static_cast<uint32_t>(data.size())));
    auto input=bgfx::createFrameBuffer(1,&color,false);
    CHECK(bgfx::isValid(input));CHECK(bgfx::isValid(color));
    if(!bgfx::isValid(input) || !bgfx::isValid(color)) {
        if(bgfx::isValid(input))bgfx::destroy(input);
        if(bgfx::isValid(color))bgfx::destroy(color);
        adapter.shutdown();return;
    }
    FrameGraph graph(adapter);FrameContext frame;RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;
    std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    PassExecContext good{adapter,pool,scene,meshes,textures,materials,7,11,31,23,frame};good.frameGraph=&graph;
    PassExecContext failed{adapter,missingPool,scene,meshes,textures,materials,7,11,31,23,frame};failed.frameGraph=&graph;
    AnalogDisplayPass working,unavailable;PresentPass present;
    ColorStylizationSettings look;look.enabled=true;look.display.enabled=true;
    working.setSettings(look);unavailable.setSettings(look);
    const auto path=std::filesystem::temp_directory_path()/(std::string("ay-present-fallback-")+backend+"-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for(int step=0;step<10;++step) {
        graph.beginFrame(31,23);graph.importExternal(FgResourceId::FinalLdrColor,input);
        PresentationStageFrame stage;stage.info=presentationStageInfo(RenderPassSlot::AnalogDisplay);stage.enabled=true;
        auto source=FgResourceId::FinalLdrColor;
        CHECK(declarePresentationStages(graph,{&stage,1},source,nullptr,true));
        graph.addPass({"Present",{source},{},true,true});
        graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
        CHECK(graph.compile());graph.markProduced(FgResourceId::FinalLdrColor);
        CHECK_FALSE(graph.directlyPresentedThisFrame());
        ClearDesc clear;clear.r=.1f;clear.g=.1f;clear.b=.1f;
        adapter.setViewRect(3,0,0,64,48);adapter.setViewFrameBuffer(3,BGFX_INVALID_HANDLE);
        adapter.setViewClear(3,clear);bgfx::touch(3);
        if(step<3) {
            CHECK(working.execute(good)==1);CHECK(graph.directlyPresentedThisFrame());
            CHECK(present.execute(good)==0);
        } else {
            CHECK(unavailable.execute(failed)==0);CHECK_FALSE(graph.directlyPresentedThisFrame());
            CHECK(graph.resolveSemantic(FgSemantic::PresentSource).idx==input.idx);
            CHECK(present.execute(good)==1);
        }
        if(step==6)CHECK(adapter.requestScreenshot(path.string()));adapter.endFrame();
    }
    // The adapter uses bgfx's default callback, which appends .tga. Renderer
    // captures above use its own PNG queue instead.
    const auto image=decodeImageFile(path.string()+".tga");CHECK(image.width==64 && image.height==48);
    size_t errors=0;
    if(image.rgba8.size()==64*48*4) {
        for(int y=11;y<34;++y)for(int x=7;x<38;++x)
            for(int c=0;c<4;++c)if(image.rgba8[size_t(y*64+x)*4+c]!=(c==3?255:(c+1)*51))++errors;
    }
    else {CHECK(false);}
    CHECK(errors==0);
    CHECK_FALSE(missingPool.compileErrorsSnapshot().empty());
    working.destroyResources(adapter);unavailable.destroyResources(adapter);present.destroyResources(adapter);
    graph.shutdown();pool.shutdown();missingPool.shutdown();bgfx::destroy(input);bgfx::destroy(color);adapter.shutdown();
#else
    SKIP_TEST("Windows D3D device required");
#endif
}

TEST_SUITE_END
