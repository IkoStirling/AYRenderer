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
#include "detail/PresentationPass.h"
#include "detail/PresentationStage.h"
#include "detail/RenderPipeline.h"
#include "detail/RenderPipelineLifecycle.h"
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

#ifdef _WIN32
namespace {
// A test-only algorithm unknown to the production stage catalog. A second
// sampler exercises shared imports through the same extension hooks as a
// resource-dependent effect. One inversion per successful stage gives an
// independent byte oracle for both odd and even chain lengths.
constexpr const char* kExtensionInvertSource=R"(
material PresentationExtensionProbe {
    texture2d inputColor
    texture2d sharedColor
    vertex {
        in pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let source = sample(inputColor, uv)
        let auxiliary = sample(sharedColor, uv)
        return vec4(1.0 - source.x, 1.0 - source.y, 1.0 - source.z, auxiliary.w)
    }
}
)";

class InvertPresentationProbe final : public ayt::render::detail::PresentationPass {
public:
    explicit InvertPresentationProbe(const ayt::render::detail::PresentationStageInfo& info)
        : PresentationPass(info) {}
    bool active() const noexcept override {return true;}
    uint32_t execute(ayt::render::detail::PassExecContext& context) override {
        using namespace ayt::render::detail;
        if(failSubmit)return 0;
        PresentationStageTargets targets;
        if(!resolveTargets(context,targets) || !_geometry.ensure(context.adapter))return 0;
        if(!_program.isValid())
            _program=context.pool.acquire(kExtensionInvertSource,"test_presentation_extension_invert_v1");
        if(!_program.isValid())return 0;
        const auto color=_program.getTextureBinding("inputColor");
        const auto extra=_program.getTextureBinding("sharedColor");
        if(color==ayt::shader::InvalidBinding || extra==ayt::shader::InvalidBinding)return 0;
        configureFullscreenPassView(context.adapter,presentationInfo().view,targets.target,
            targets.x,targets.y,context.viewportWidth,context.viewportHeight);
        _geometry.bind(context.adapter);
        _program.setTexture(_program.getTextureStage(color),color,toShaderTexture(targets.input));
        _program.setTexture(_program.getTextureStage(extra),extra,
            toShaderTexture(context.adapter.getFboAttachment(sharedInput,0)));
        context.adapter.setStateDepthTestAlways();
        _program.submit({presentationInfo().view,0});publishOutput(context);return 1;
    }
    void destroyResources(ayt::render::detail::BGFXAdapter& adapter) override {
        ++cleanups;_program.reset();_geometry.destroy(adapter);
    }
    bool resourcesReleased() const noexcept {return !_program.isValid() && !_geometry.isReady();}
    unsigned cleanups=0;
    bgfx::FrameBufferHandle sharedInput=BGFX_INVALID_HANDLE;
    bool failSubmit=false;
protected:
    void preparePresentationResources(ayt::render::detail::PresentationFrameContext&,
        ayt::render::detail::PresentationStageFrame& stage) override {
        using namespace ayt::render::detail;
        stage.extraReads.push_back(FgResourceId::FeedbackHistory);
        stage.imports.push_back({FgResourceId::FeedbackHistory,sharedInput});
    }
private:
    ayt::render::detail::FullscreenPassGeometry _geometry;
    ayt::shader::ShaderResource _program;
};
}
#endif

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
        graph.addPass({"Present",{source},{},true,true,static_cast<int16_t>(RenderPassSlot::Present)});
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

TEST_CASE(unknown_color_stages_execute_up_to_twenty_with_shared_inputs_failures_and_texture_consumers)
{
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0])SKIP_TEST("Isolated D3D11/12 device required");
    using namespace ayt::render::detail;
    HWND window=CreateWindowExW(0,L"STATIC",L"Presentation extension proof",WS_POPUP,
        0,0,64,48,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);if(!window)return;
    struct WindowCleanup{HWND h;~WindowCleanup(){DestroyWindow(h);}} cleanup{window};
    BGFXAdapter adapter;BGFXInitParams init;init.nativeWindowHandle=window;
    init.width=64;init.height=48;init.msaa=0;init.vsync=false;
    init.backend=std::string(backend)=="d3d12"?Backend::Direct3D12:Backend::Direct3D11;
    CHECK(adapter.initialize(init));if(!adapter.isInitialized())return;
    ayt::shader::ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT});
    pool.resolvePlatformFromRenderer();
    auto proofProgram=pool.acquire(kExtensionInvertSource,"test_presentation_extension_invert_v1");
    CHECK(proofProgram.isValid());
    if(!proofProgram.isValid()) {
        for(const auto& error:pool.lastCompileErrors())std::cerr<<error<<'\n';
        pool.shutdown();adapter.shutdown();return;
    }
    proofProgram.reset();
    constexpr size_t count=20;
    std::array<std::string,count> names;
    std::array<PresentationStageInfo,count> infos;
    std::array<InvertPresentationProbe*,count> probes{};
    RenderPipeline pipeline;size_t output=0;uint16_t view=0;
    for(size_t index=0;index<count;++index) {
        while(output<static_cast<size_t>(FgResourceId::Count)
            && (output==static_cast<size_t>(FgResourceId::FinalLdrColor)
                || contractResourceFor(static_cast<FgResourceId>(output))==RenderPassResourceId::Count))++output;
        while(view<kRenderViewCapacity && (!canReserveMainRenderView(view) || isMainRenderViewReserved(view)))++view;
        CHECK(output<static_cast<size_t>(FgResourceId::Count));CHECK(view<kRenderViewCapacity);
        if(output>=static_cast<size_t>(FgResourceId::Count) || view>=kRenderViewCapacity){adapter.shutdown();return;}
        names[index]="UnregisteredGpuStage"+std::to_string(index);
        const auto resource=static_cast<FgResourceId>(output++);
        infos[index]={static_cast<RenderPassSlot>(200+index),names[index],resource,
            {contractResourceFor(resource),RenderPassResourceLifetime::Transient,RenderPassResourceFormat::RGBA8},
            static_cast<uint8_t>(view++),static_cast<uint16_t>(10+index),PresentationStageInputs::Color,
            true,false,names[index],names[index],true};
        auto pass=std::make_unique<InvertPresentationProbe>(infos[index]);probes[index]=pass.get();
        pipeline.addPass(std::move(pass));
    }
    auto present=std::make_unique<PresentPass>();
    pipeline.addPass(RenderPassSlot::Present,std::move(present));
    // The fixture owns a full permutation: all probe Views must precede the
    // production Present View, irrespective of their unused numeric IDs.
    std::array<bgfx::ViewId,kRenderViewCapacity> remap{};
    std::array<bool,kRenderViewCapacity> used{};size_t rank=0;
    remap[rank++]=3;used[3]=true;
    for(const auto& info:infos){remap[rank++]=info.view;used[info.view]=true;}
    remap[rank++]=PresentPass::kPresentViewId;used[PresentPass::kPresentViewId]=true;
    for(size_t id=0;id<used.size();++id)if(!used[id])remap[rank++]=static_cast<bgfx::ViewId>(id);
    CHECK(rank==remap.size());adapter.setViewOrder(0,static_cast<uint16_t>(remap.size()),remap.data());
    std::vector<uint8_t> pixels(31*23*4);
    for(int y=0;y<23;++y)for(int x=0;x<31;++x) {
        const size_t p=size_t(y*31+x)*4;
        pixels[p]=uint8_t((x*17+y*13)%256);pixels[p+1]=uint8_t((x*7+y*29)%256);
        pixels[p+2]=uint8_t((x*31+y*3)%256);pixels[p+3]=255;
    }
    auto texture=bgfx::createTexture2D(31,23,false,1,bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT|BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(pixels.data(),static_cast<uint32_t>(pixels.size())));
    auto input=bgfx::isValid(texture)?bgfx::createFrameBuffer(1,&texture,false):bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    CHECK(bgfx::isValid(texture));CHECK(bgfx::isValid(input));
    if(!bgfx::isValid(texture) || !bgfx::isValid(input)) {
        if(bgfx::isValid(input))bgfx::destroy(input);
        if(bgfx::isValid(texture))bgfx::destroy(texture);
        pool.shutdown();adapter.shutdown();return;
    }
    for(auto* probe:probes)probe->sharedInput=input;
    FrameGraph graph(adapter);FrameContext frame;RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;
    std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    PassExecContext context{adapter,pool,scene,meshes,textures,materials,7,11,31,23,frame};context.frameGraph=&graph;
    PresentationFrameContext preparation{adapter,pool,frame,7,11,31,23,true,input};
    std::vector<PresentationStageFrame> stages;
    const auto captures=std::filesystem::temp_directory_path()/(std::string("ay-extension-proof-")+backend+"-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(captures);
    for(const size_t enabled:{0u,1u,5u,10u,20u})for(int mode=0;mode<7;++mode) {
        // 0 separate; 1 integrated; 2 extra reader; 3 semantic; 4 disabled
        // reader; 5 middle failure; 6 terminal failure.
        const int failed=enabled && mode>=5?int(mode==5?enabled/2:enabled-1):-1;
        for(size_t index=0;index<count;++index) {
            probes[index]->setEnabled(index<enabled);probes[index]->failSubmit=int(index)==failed;
        }
        const auto capture=captures/(std::to_string(enabled)+"-"+std::to_string(mode));
        for(int tick=0;tick<10;++tick) {
            preparePresentationStages(pipeline,preparation,stages);
            graph.beginFrame(31,23);graph.importExternal(FgResourceId::FinalLdrColor,input);
            auto source=FgResourceId::FinalLdrColor;
            CHECK(declarePresentationStages(graph,stages,source,nullptr,mode!=0));
            if(mode==2 || mode==4)graph.addPass({"FutureTextureReader",{source},{},mode==2,true,220});
            if(mode==3)graph.setResolvedSemantic(FgSemantic::BloomSource,source);
            graph.addPass({"Present",{source},{},true,true,static_cast<int16_t>(RenderPassSlot::Present)});
            graph.setExecutionEligibility(RenderPassSlot::Present,true);
            graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
            CHECK(graph.compile());graph.markProduced(FgResourceId::FinalLdrColor);
            const bool direct=enabled && mode!=0 && mode!=2 && mode!=3;
            CHECK(graph.isDirectPresentationOutput(source)==direct);
            CHECK(graph.stats().physicalTargets==enabled-(direct?1:0));
            ClearDesc clear;clear.r=.1f;clear.g=.2f;clear.b=.3f;
            adapter.setViewRect(3,0,0,64,48);adapter.setViewFrameBuffer(3,BGFX_INVALID_HANDLE);
            adapter.setViewClear(3,clear);bgfx::touch(3);
            const bool presented=direct && failed!=int(enabled-1);
            CHECK(pipeline.executeAll(context)==enabled-(failed>=0?1:0)+(presented?0:1));
            CHECK(graph.directlyPresentedThisFrame()==presented);
            if(enabled && !direct) {
                CHECK(bgfx::isValid(graph.resolve(source)));
                CHECK(bgfx::isValid(adapter.getFboAttachment(graph.resolve(source),0)));
            }
            if(tick==6)CHECK(adapter.requestScreenshot(capture.string()));
            adapter.endFrame();finishPresentationStages(pipeline,nullptr);
        }
        const auto image=decodeImageFile(capture.string()+".tga");
        CHECK(image.width==64 && image.height==48);CHECK(image.rgba8.size()==64*48*4);
        const bool inverted=(enabled-(failed>=0?1:0))%2!=0;size_t differences=0;
        if(image.rgba8.size()==64*48*4)for(int y=0;y<23;++y)for(int x=0;x<31;++x)
            for(size_t channel=0;channel<4;++channel) {
                const auto authored=pixels[size_t(y*31+x)*4+channel];
                const uint8_t expected=inverted && channel<3?uint8_t(255-authored):authored;
                if(image.rgba8[size_t((y+11)*64+x+7)*4+channel]!=expected)++differences;
            }
        std::cout<<"[Extension GPU] backend="<<backend<<" stages="<<enabled<<" mode="<<mode
            <<" differentBytes="<<differences<<'\n';
        CHECK(differences==0);
    }
    destroyPipelineGpuResources(pipeline,adapter);
    for(const auto* probe:probes){CHECK(probe->cleanups==1);CHECK(probe->resourcesReleased());}
    destroyPipelineGpuResources(pipeline,adapter);
    for(const auto* probe:probes){CHECK(probe->cleanups==2);CHECK(probe->resourcesReleased());}
    pipeline.clear();graph.shutdown();pool.shutdown();bgfx::destroy(input);bgfx::destroy(texture);adapter.shutdown();
#else
    SKIP_TEST("Windows D3D device required");
#endif
}

TEST_SUITE_END
