#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <AYRenderer.h>
#include <AYRenderer/EmissivePointCloud.h>
#include <AYRenderer/FixtureEmitters.h>
#include <AYRenderer/FixtureHousings.h>
#include <AYRenderer/PbrShaderSources.h>
#include <numeric>
#include <algorithm>
#include <AYRenderer/UIRenderBackend.h>
#include <AYTest.h>
#include "detail/BGFXAdapter.h"
#include "detail/CameraViewsTestAccess.h"
#include <array>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
TEST_SUITE(CameraViewsGpu)
TEST_CASE(independent_camera_layers_outputs_and_resize_are_verified_by_gpu_readback) {
#ifdef _WIN32
    if (!std::getenv("AY_CAMERA_GPU_TEST")) SKIP_TEST("Requires an isolated Windows D3D11 device");
    HWND window=CreateWindowExW(0,L"STATIC",L"Camera view acceptance",WS_OVERLAPPED,
        0,0,128,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window!=nullptr); if(!window) return;
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc; desc.windowHandle=window; desc.backend=ayt::render::Backend::Direct3D11;
    desc.width=128; desc.height=128; desc.vsync=false; desc.msaa=0;
    CHECK(renderer.initialize(desc));
    if (!renderer.isInitialized()) { DestroyWindow(window); return; }
    auto* adapter=ayt::render::detail::CameraViewsTestAccess::adapter(renderer);
    const uint64_t required=BGFX_CAPS_TEXTURE_READ_BACK | BGFX_CAPS_TEXTURE_BLIT;
    CHECK((bgfx::getCaps()->supported & required)==required);
    const char* source=R"(material CameraAcceptance {
        property baseColor = vec4(1.0, 0.0, 0.0, 1.0)
        vertex {
            in pos : position
            return modelViewProjection * vec4(pos, 1.0)
        }
        fragment { return baseColor }
    })";
    auto red=renderer.createMaterialFromPhoskia(source,"camera_gpu_red");
    auto green=renderer.createMaterialFromPhoskia(source,"camera_gpu_green");
    CHECK(red.isValid()); CHECK(green.isValid());
    renderer.setMaterialColor(red,"baseColor",1,0,0,1); renderer.setMaterialColor(green,"baseColor",0,1,0,1);
    auto mesh=renderer.createUnitCube(); CHECK(mesh.isValid());
    ayt::render::RenderScene scene;
    ayt::render::DrawItem draw; draw.mesh=mesh; draw.material=red; draw.cameraLayerMask=1; scene.add(draw);
    draw.material=green; draw.cameraLayerMask=2; scene.add(draw);
    auto a=renderer.createCameraOutput(32,32), b=renderer.createCameraOutput(32,32);
    CHECK(a.id && b.id);
    ayt::render::UIRenderBackend ui; CHECK(ui.initialize(renderer)); ui.setFramebufferSize(128,128);
    ayt::ui::IRenderBackend::LayerDesc layerDesc;
    layerDesc.logicalBounds=ayt::math::FRectangle(0,0,8,8);
    std::vector<ayt::ui::IRenderBackend::LayerHandle> layers;
    for(int index=0; index<40; ++index) {
        auto layer=ui.createLayer(layerDesc); CHECK(layer.isValid()); layers.push_back(layer);
    }
    auto readback=bgfx::createTexture2D(32,32,false,1,bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
    CHECK(bgfx::isValid(readback));
    std::array<ayt::render::CameraRenderView,2> views;
    views[0].scene=&scene; views[0].position={0,0,-3};
    views[0].view=ayt::math::lh::lookAt({0,0,-3},{0,0,0},{0,1,0});
    views[0].projection=ayt::math::lh::perspective(ayt::math::radians(50.f),1,.1f,100);
    views[0].width=32; views[0].height=32; views[0].layerMask=1; views[0].output=a;
    views[1]=views[0]; views[1].id=1; views[1].layerMask=2; views[1].output=b;
    views[1].position={3,0,-3};
    views[1].view=ayt::math::lh::lookAt(views[1].position,{0,0,0},{0,1,0});
    views[1].projection=ayt::math::lh::ortho(-2,2,-2,2,.1f,100);
    for (int step=0; step<3; ++step) {
        renderer.beginFrame(); ui.beginFrame();
        for(auto layer:layers) {
            ayt::ui::IRenderBackend::LayerPaint paint; paint.fullRedraw=true;
            CHECK(ui.beginLayerPaint(layer,paint));
            ui.drawRect(layerDesc.logicalBounds,{0,0,1,1}); ui.endLayerPaint(layer);
        }
        ui.endFrame(); CHECK(renderer.renderViews(views)); renderer.endFrame();
    }
    auto checkOutput=[&](ayt::render::CameraOutputHandle output,bool isRed) {
        const auto fbo=bgfx::FrameBufferHandle{ayt::render::detail::CameraViewsTestAccess::framebuffer(renderer,output)};
        const auto color=adapter->getFboAttachment(fbo,0);
        bgfx::blit(112,readback,0,0,color);
        std::array<uint8_t,32*32*4> pixels{};
        const auto ready=bgfx::readTexture(readback,pixels.data());
        for(int frame=0; frame<120 && adapter->gpuFrameCounter()<ready; ++frame) renderer.endFrame();
        CHECK(adapter->gpuFrameCounter()>=ready);
        const size_t center=(16*32+16)*4;
        std::cerr << "[Camera GPU] red=" << isRed << " center=" << int(pixels[center]) << ',' << int(pixels[center+1]) << ',' << int(pixels[center+2]) << '\n';
        CHECK(pixels[center+(isRed?0:1)]>240); CHECK(pixels[center+(isRed?1:0)]<10); CHECK(pixels[center+2]<10);
    };
    checkOutput(a,true); checkOutput(b,false);
    renderer.resize(256,128);
    renderer.beginFrame(); CHECK(renderer.renderViews(views)); renderer.endFrame();
    checkOutput(a,true); checkOutput(b,false);
    renderer.destroyCameraOutput(a); CHECK_FALSE(renderer.cameraOutputTexture(a).isValid());
    renderer.destroyCameraOutput(b); bgfx::destroy(readback);
    for(auto layer:layers) ui.releaseLayer(layer);
    ui.shutdown();
    renderer.destroyMesh(mesh); renderer.destroyMaterial(red); renderer.destroyMaterial(green);
    renderer.shutdown(); DestroyWindow(window);
#else
    SKIP_TEST("Windows D3D11 device required");
#endif
}
TEST_SUITE_END

TEST_SUITE(EmissivePointsGpu)
TEST_CASE(single_instanced_point_matches_cpu_quad_in_a_real_view) {
#ifdef _WIN32
    if (!std::getenv("AY_POINT_GPU_TEST")) SKIP_TEST("Requires an isolated Windows D3D11 device");
    HWND window=CreateWindowExW(0,L"STATIC",L"Emissive point GPU regression",WS_OVERLAPPED,
        0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window!=nullptr);if(!window)return;
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;desc.windowHandle=window;desc.backend=ayt::render::Backend::Direct3D11;
    desc.width=64;desc.height=64;desc.vsync=false;desc.msaa=0;
    CHECK(renderer.initialize(desc));
    if(!renderer.isInitialized()){DestroyWindow(window);return;}
    CHECK((bgfx::getCaps()->supported&BGFX_CAPS_INSTANCING)!=0);
    auto* adapter=ayt::render::detail::CameraViewsTestAccess::adapter(renderer);
    auto output=renderer.createCameraOutput(64,64);CHECK(output.id!=0);
    auto readback=bgfx::createTexture2D(64,64,false,1,bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST);
    CHECK(bgfx::isValid(readback));
    const ayt::math::FVector3 eye{0,0,-3};
    const auto view=ayt::math::lh::lookAt(eye,{0,0,0},{0,1,0});
    const auto projection=ayt::math::lh::perspective(ayt::math::radians(50.f),1,.1f,100);
    ayt::render::EmissivePoint point;point.id=7;point.color={1,0,0};point.intensity=4;
    point.radius=.4f;
    ayt::render::EmissivePointCloud cloud;
    auto sample=[&](bool preferInstances) {
        ayt::render::RenderScene scene;std::string error;
        ayt::render::EmissivePointSettings settings;settings.preferInstancing=preferInstances;
        CHECK(cloud.append(renderer,scene,std::span<const ayt::render::EmissivePoint>(&point,1),
            view,projection,64,64,settings,&error));
        CHECK(error.empty());CHECK(cloud.stats().instanced==preferInstances);
        ayt::render::CameraRenderView camera;camera.scene=&scene;camera.position=eye;
        camera.view=view;camera.projection=projection;camera.width=camera.height=64;camera.output=output;
        const std::array<ayt::render::CameraRenderView,1> views{camera};
        for(int frame=0;frame<3;++frame){renderer.beginFrame();CHECK(renderer.renderViews(views));renderer.endFrame();}
        const auto fbo=bgfx::FrameBufferHandle{ayt::render::detail::CameraViewsTestAccess::framebuffer(renderer,output)};
        const auto color=adapter->getFboAttachment(fbo,0);
        bgfx::blit(112,readback,0,0,color);
        std::array<uint8_t,64*64*4> pixels{};
        const auto ready=bgfx::readTexture(readback,pixels.data());
        for(int frame=0;frame<120&&adapter->gpuFrameCounter()<ready;++frame)renderer.endFrame();
        CHECK(adapter->gpuFrameCounter()>=ready);
        const size_t center=(32*64+32)*4;
        return std::array<uint8_t,4>{pixels[center],pixels[center+1],pixels[center+2],pixels[center+3]};
    };
    const auto instanced=sample(true),cpu=sample(false);
    CHECK(instanced[0]>20);CHECK(cpu[0]>20);
    CHECK(instanced[0]>instanced[1]);CHECK(cpu[0]>cpu[1]);
    CHECK(std::abs(int(instanced[0])-int(cpu[0]))<35);
    renderer.destroyCameraOutput(output);bgfx::destroy(readback);
    renderer.shutdown();DestroyWindow(window);
#else
    SKIP_TEST("Windows D3D11 device required");
#endif
}
TEST_SUITE_END
}

namespace {
#ifdef _WIN32
struct FixtureEmitterGpuFixture {
    HWND window=nullptr;
    ayt::render::Renderer renderer;
    bgfx::TextureHandle readback=BGFX_INVALID_HANDLE;
    ayt::render::FixtureEmitters emitters;
    ayt::render::FixtureHousings housings;
    ayt::render::MeshHandle pane;
    ayt::render::MaterialHandle glass;
    ayt::render::PerformanceLightStats lastTransparentStats;
    ayt::render::SceneLights dark;
    ayt::render::MeshHandle cube;
    ayt::render::MaterialHandle black;
    ~FixtureEmitterGpuFixture(){if(bgfx::isValid(readback))bgfx::destroy(readback);renderer.shutdown();if(window)DestroyWindow(window);}
    bool initialize(const char* backend){
        window=CreateWindowExW(0,L"STATIC",L"Fixture aperture acceptance",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!window)return false;
        ayt::render::InitDesc desc;desc.windowHandle=window;desc.width=desc.height=64;desc.vsync=false;desc.msaa=0;
        desc.backend=std::string(backend)=="d3d12"?ayt::render::Backend::Direct3D12:ayt::render::Backend::Direct3D11;
        if(!renderer.initialize(desc))return false;
        if((bgfx::getCaps()->supported&(BGFX_CAPS_TEXTURE_READ_BACK|BGFX_CAPS_TEXTURE_BLIT))!=(BGFX_CAPS_TEXTURE_READ_BACK|BGFX_CAPS_TEXTURE_BLIT))return false;
        renderer.configurePipeline(ayt::render::RenderPipelineDesc::makeDeferred());renderer.setAmbientStrength(0);
        dark.add(ayt::render::Light::directional({0,-1,0},{0,0,0}));renderer.setSceneLights(&dark);
        renderer.setPostProcessExposure(1);renderer.setAutoExposureEnabled(false);renderer.setPostProcessBloomThreshold(1);
        renderer.setMainCameraLookAtPerspective({0,0,-3},{0,0,0},{0,1,0},50,1,.1f,100);
        cube=renderer.createTexturedUnitCube();black=renderer.createMaterialFromPhoskia(ayt::render::kPbrPhoskiaSource,"emitter_black_occluder");
        renderer.setMaterialColor(black,"baseColor",0,0,0,1);renderer.setMaterialFloat(black,"roughness",1);
        readback=bgfx::createTexture2D(64,64,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST);
        return bgfx::isValid(readback)&&cube.isValid()&&black.isValid();
    }
    std::array<uint8_t,64*64*4> sampleLight(const ayt::render::PerformanceLight& light,bool volume,uint8_t downsample=1,const ayt::render::VolumetricSettings* overrideFog=nullptr){
        using namespace ayt::render;renderer.setPostProcessBloomStrength(0);
        VolumetricSettings fog;fog.enabled=volume;fog.downsample=downsample;fog.density=.1f;fog.scattering=1;fog.anisotropy=0;fog.steps=64;fog.debugView=2;if(overrideFog)fog=*overrideFog;renderer.setVolumetricSettings(fog);
        CHECK(renderer.setPerformanceLights({&light,1}));RenderScene scene;
        renderer.setMaterialColor(black,"baseColor",1,1,1,1);scene.add(cube,black,ayt::math::scale(1.f,1.f,.1f));
        for(int i=0;i<3;++i){renderer.beginFrame({0,0,0,1});renderer.render(scene);renderer.endFrame();}
        const auto color=bgfx::TextureHandle{ayt::render::detail::CameraViewsTestAccess::finalColor(renderer)};CHECK(bgfx::isValid(color));
        std::array<uint8_t,64*64*4> pixels{};if(bgfx::isValid(color)){
            bgfx::blit(112,readback,0,0,color);const auto ready=bgfx::readTexture(readback,pixels.data());auto* adapter=ayt::render::detail::CameraViewsTestAccess::adapter(renderer);
            for(int i=0;i<120&&adapter->gpuFrameCounter()<ready;++i)renderer.endFrame();CHECK(adapter->gpuFrameCounter()>=ready);
        }
        CHECK(renderer.setPerformanceLights({}));return pixels;
    }
    std::array<uint8_t,64*64*4> sampleShadow(float blockerT,float radius,bool area,bool shadows,bool volume=false,bool zero=false){
        using namespace ayt::render;renderer.setPostProcessBloomStrength(0);renderer.setPostProcessGamma(1);renderer.setPostProcessExposure(2);
        VolumetricSettings fog;fog.enabled=volume;fog.density=.08f;fog.scattering=1;fog.anisotropy=0;fog.steps=32;fog.debugView=2;renderer.setVolumetricSettings(fog);
        PerformanceLight light;light.id=7;light.light=Light::spot({-1,0,-2},ayt::math::FVector3(1,0,1.95f).normalize(),10,zero?0.f:2.f,.9f,.7f,{1,1,1});
        light.light.castShadow=shadows;light.light.shadowBias=.0001f;light.apertureRadius=radius;light.areaShadow=area;light.areaShadowBias=.001f;light.intensityUnit=LightIntensityUnit::Candela;
        CHECK(renderer.setPerformanceLights({&light,1}));RenderScene scene;renderer.setMaterialColor(black,"baseColor",1,1,1,1);
        DrawItem receiver;receiver.mesh=cube;receiver.material=black;receiver.world=ayt::math::scale(1.2f,1.2f,.05f);receiver.shadowFlags=ShadowFlags::Receive;scene.add(receiver);
        scene.add(cube,black,ayt::math::translate(-1+blockerT,0.f,-2+1.95f*blockerT)*ayt::math::scale(.12f,.12f,.04f));
        for(int i=0;i<3;++i){renderer.beginFrame({0,0,0,1});renderer.render(scene);renderer.endFrame();}
        if(shadows&&!zero)CHECK(renderer.performanceLightStats().shadowLights==1);
        const auto color=bgfx::TextureHandle{ayt::render::detail::CameraViewsTestAccess::finalColor(renderer)};CHECK(bgfx::isValid(color));std::array<uint8_t,64*64*4> pixels{};
        if(bgfx::isValid(color)){bgfx::blit(112,readback,0,0,color);const auto ready=bgfx::readTexture(readback,pixels.data());auto* adapter=ayt::render::detail::CameraViewsTestAccess::adapter(renderer);for(int i=0;i<120&&adapter->gpuFrameCounter()<ready;++i)renderer.endFrame();CHECK(adapter->gpuFrameCounter()>=ready);}
        CHECK(renderer.setPerformanceLights({}));return pixels;
    }
    std::array<uint8_t,64*64*4> sampleTransparent(float alpha,float intensity,uint16_t drawBudget=128,float paneZ=0,bool rear=false,bool blocker=false,ayt::render::LightPattern pattern=ayt::render::LightPattern::Open,std::shared_ptr<const ayt::render::IesProfile> ies={}){
        using namespace ayt::render;renderer.setPostProcessBloomStrength(0);renderer.setPostProcessGamma(1);renderer.setPostProcessExposure(1);renderer.setVolumetricSettings({});
        if(!pane.isValid()){
            struct V{float x,y,z,nx,ny,nz,u,v,tx,ty,tz,tw;};
            const V vertices[]={{-.8f,-.8f,0,0,0,-1,0,0,1,0,0,1},{-.8f,.8f,0,0,0,-1,0,1,1,0,0,1},{.8f,.8f,0,0,0,-1,1,1,1,0,0,1},{.8f,-.8f,0,0,0,-1,1,0,1,0,0,1}};
            const uint16_t indices[]={0,1,2,0,2,3};auto layout=VertexLayoutDesc::position3Normal3TexCoord2();CHECK(layout.add({VertexAttribute::Tangent,4,VertexComponentType::Float,false}));
            pane=renderer.createMesh(vertices,4,layout,indices,6);glass=renderer.createMaterialFromPhoskia(kPbrPhoskiaSource,"transparent_performance_probe");CHECK(pane.isValid());CHECK(glass.isValid());
            renderer.setMaterialBlendMode(glass,BlendMode::Alpha);renderer.setMaterialColor(glass,"baseColor",1,1,1,1);renderer.setMaterialFloat(glass,"roughness",1);renderer.setMaterialFloat(glass,"metallic",0);
        }
        renderer.setMaterialFloat(glass,"opacity",alpha);
        PerformanceLight light;light.id=7;light.light=Light::spot({0,0,-2},{0,0,1},10,intensity,.95f,.8f,{1,0,0});light.volumetric=0;light.intensityUnit=LightIntensityUnit::Candela;light.pattern=pattern;
        TextureHandle atlas;if(ies){atlas=createIesTexture(renderer,*ies);CHECK(atlas.isValid());light.iesProfile=ies;light.iesTexture=atlas;}
        PerformanceLightBudget budget;budget.transparentDraws=drawBudget;CHECK(renderer.setPerformanceLights({&light,1},budget));
        RenderScene scene;scene.add(pane,glass,ayt::math::translate(0.f,0.f,paneZ));if(rear)scene.add(pane,glass,ayt::math::translate(0.f,0.f,paneZ+1));
        if(blocker){renderer.setMaterialColor(black,"baseColor",0,0,0,1);scene.add(cube,black,ayt::math::translate(0.f,0.f,-.5f)*ayt::math::scale(1.f,1.f,.1f));}
        for(int i=0;i<3;++i){renderer.beginFrame({0,0,0,1});renderer.render(scene);renderer.endFrame();}
        lastTransparentStats=renderer.performanceLightStats();const auto color=bgfx::TextureHandle{ayt::render::detail::CameraViewsTestAccess::finalColor(renderer)};CHECK(bgfx::isValid(color));std::array<uint8_t,64*64*4> pixels{};
        if(bgfx::isValid(color)){bgfx::blit(112,readback,0,0,color);const auto ready=bgfx::readTexture(readback,pixels.data());auto* adapter=ayt::render::detail::CameraViewsTestAccess::adapter(renderer);for(int i=0;i<120&&adapter->gpuFrameCounter()<ready;++i)renderer.endFrame();CHECK(adapter->gpuFrameCounter()>=ready);}
        CHECK(renderer.setPerformanceLights({}));if(atlas.isValid())renderer.destroyTexture(atlas);return pixels;
    }
    std::array<uint8_t,64*64*4> sampleIes(const std::shared_ptr<const ayt::render::IesProfile>& profile,bool volume){
        using namespace ayt::render;
        PerformanceLight light;light.id=7;light.light=Light::spot({0,0,-2},{0,0,1},10,1,.98f,.75f,{1,0,0});light.intensityUnit=LightIntensityUnit::Candela;
        TextureHandle atlas;if(profile){atlas=createIesTexture(renderer,*profile);CHECK(atlas.isValid());light.iesProfile=profile;light.iesTexture=atlas;}
        const auto pixels=sampleLight(light,volume);if(atlas.isValid())renderer.destroyTexture(atlas);return pixels;
    }
    std::array<uint8_t,64*64*4> sample(int mode,bool bloom=false,float density=0,ayt::render::LensEffectsSettings lens={},bool housing=false,ayt::render::FixtureHousingSettings body={},bool lampOff=false){
        renderer.setLensEffects(lens);
        renderer.setPostProcessBloomStrength(bloom?1.f:0.f);
        ayt::render::VolumetricSettings fog;fog.enabled=density>0;fog.density=density;renderer.setVolumetricSettings(fog);
        ayt::render::PerformanceLight light;light.id=7;light.light=ayt::render::Light::spot({0,0,0},{0,0,-1},10,10,.95f,.8f,{1,0,0});light.apertureRadius=.25f;
        if(lampOff)light.light.intensity=0;
        if(mode==5)light.light.position={.65f,0,0};
        if(mode==1)light.light.spotDirection={.98f,0,-.2f};if(mode==2)light.light.spotDirection={0,0,1};
        ayt::math::Float4x4 view,projection;CHECK(renderer.mainCameraMatrices(view,projection));
        ayt::render::RenderScene scene;std::string error;
        if(housing)CHECK(housings.append(renderer,scene,{&light,1},body,&error));
        CHECK(emitters.append(renderer,scene,{&light,1},view,projection,64,64,{},&error));CHECK(error.empty());
        if(mode==3||mode==4)scene.add(cube,black,ayt::math::translate(mode==4?-1.5f:0.f,0.f,-1.f)*ayt::math::scale(mode==4?1.5f:3.f,3.f,.2f));
        for(int i=0;i<3;++i){renderer.beginFrame({0,0,0,1});renderer.render(scene);renderer.endFrame();}
        const auto color=bgfx::TextureHandle{ayt::render::detail::CameraViewsTestAccess::finalColor(renderer)};CHECK(bgfx::isValid(color));
        std::array<uint8_t,64*64*4> pixels{};if(!bgfx::isValid(color))return pixels;
        bgfx::blit(112,readback,0,0,color);const auto ready=bgfx::readTexture(readback,pixels.data());
        auto* adapter=ayt::render::detail::CameraViewsTestAccess::adapter(renderer);
        for(int i=0;i<120&&adapter->gpuFrameCounter()<ready;++i)renderer.endFrame();CHECK(adapter->gpuFrameCounter()>=ready);
        return pixels;
    }
};
double emitterRedEnergy(const std::array<uint8_t,64*64*4>& pixels){double sum=0;for(size_t i=0;i<pixels.size();i+=4)sum+=pixels[i];return sum;}
#endif
TEST_SUITE(FixtureEmittersGpu)
TEST_CASE(real_main_view_aperture_is_directional_depth_occluded_and_extinguished_by_fog){
#ifdef _WIN32
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device emitter acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    const auto front=f.sample(0);const double energy=emitterRedEnergy(front);CHECK(energy>1000);CHECK(f.emitters.stats().prepared==1);
    const auto side=f.sample(1);CHECK(emitterRedEnergy(side)<energy*.2);
    const auto back=f.sample(2);CHECK(f.emitters.stats().backFacing==1);CHECK(emitterRedEnergy(back)<energy*.01);
    const auto hidden=f.sample(3);CHECK(emitterRedEnergy(hidden)<energy*.02);
    const auto partial=f.sample(4);std::fprintf(stderr,"[Emitter probe] front=%.1f side=%.1f back=%.1f hidden=%.1f partial=%.1f\n",energy,emitterRedEnergy(side),emitterRedEnergy(back),emitterRedEnergy(hidden),emitterRedEnergy(partial));CHECK(emitterRedEnergy(partial)>energy*.2);CHECK(emitterRedEnergy(partial)<energy*.8);
    const auto fogged=f.sample(0,false,.5f);CHECK(emitterRedEnergy(fogged)<energy*.95);CHECK(emitterRedEnergy(fogged)>0);
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_CASE(real_bloom_spreads_visible_aperture_but_does_not_leak_through_occluder){
#ifdef _WIN32
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device emitter acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    const auto clear=f.sample(0,false),bloom=f.sample(0,true),blocked=f.sample(3,true);
    const auto halo=[](const auto& pixels){double sum=0;for(int y=22;y<42;++y)for(int x=41;x<52;++x)sum+=pixels[(y*64+x)*4];return sum;};
    CHECK(halo(bloom)>halo(clear)+20);CHECK(emitterRedEnergy(blocked)<emitterRedEnergy(bloom)*.02);
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_CASE(real_ies_distribution_matches_surface_and_volume_directions){
#ifdef _WIN32
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device IES acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    auto profile=std::make_shared<ayt::render::IesProfile>();
    CHECK(ayt::render::IesProfile::parse("IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 3 2 1 2 0 0 0\n1 1 1\n0 45 90\n0 180\n100 100 100\n0 0 0\n",*profile));
    const auto halves=[](const auto& pixels){std::array<double,2> sum{};for(int y=15;y<49;++y)for(int x=14;x<50;++x)sum[x<32?0:1]+=pixels[(y*64+x)*4];return sum;};
    for(bool volume:{false,true}){
        const auto baseline=halves(f.sampleIes({},volume)),ies=halves(f.sampleIes(profile,volume));
        std::fprintf(stderr,"[IES probe] volume=%d uniform=%.0f/%.0f ies=%.0f/%.0f\n",int(volume),baseline[0],baseline[1],ies[0],ies[1]);
        CHECK(baseline[0]>100);CHECK(baseline[1]>100);CHECK(std::abs(baseline[0]-baseline[1])<baseline[0]*.1);
        CHECK(std::max(ies[0],ies[1])>std::min(ies[0],ies[1])*1.15);CHECK(ies[0]+ies[1]<baseline[0]+baseline[1]);
        // Project the measured h=0 (+X) direction through the actual camera.
        ayt::math::Float4x4 view,projection;CHECK(f.renderer.mainCameraMatrices(view,projection));
        const auto clip=projection*view*ayt::math::FVector4(.5f,0,0,1);
        CHECK(ies[clip.x>0?1:0]>ies[clip.x>0?0:1]);
    }
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_CASE(real_prism_lobes_rotate_and_distance_focus_changes_pattern_contrast){
#ifdef _WIN32
    using namespace ayt::render;
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device optical acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    PerformanceLight p;p.id=7;p.light=Light::spot({0,0,-2},{0,0,1},10,1,std::cos(.04f),std::cos(.08f),{1,0,0});p.apertureRadius=.02f;
    ayt::math::Float4x4 view,projection;CHECK(f.renderer.mainCameraMatrices(view,projection));
    const auto probe=[&](const auto& pixels,float x,float y){const auto clip=projection*view*ayt::math::FVector4(x,y,-.1f,1);const int cx=int((clip.x/clip.w*.5f+.5f)*64),cy=int((.5f-clip.y/clip.w*.5f)*64);double sum=0;
        for(int yy=cy-1;yy<=cy+1;++yy)for(int xx=cx-1;xx<=cx+1;++xx)if(xx>=0&&xx<64&&yy>=0&&yy<64)sum+=pixels[(yy*64+xx)*4];return sum;};
    for(bool volume:{false,true}){
        auto zero=p;zero.light.intensity=0;const auto baseline=f.sampleLight(zero,volume);
        const auto one=f.sampleLight(p,volume);p.prismCount=2;p.prismAngle=.2f;p.prismRotation=0;const auto horizontal=f.sampleLight(p,volume);
        p.prismRotation=1.570796327f;const auto vertical=f.sampleLight(p,volume);
        const auto hx=probe(horizontal,.385f,0)+probe(horizontal,-.385f,0),hy=probe(horizontal,0,.385f)+probe(horizontal,0,-.385f);
        const auto vx=probe(vertical,.385f,0)+probe(vertical,-.385f,0),vy=probe(vertical,0,.385f)+probe(vertical,0,-.385f);
        std::fprintf(stderr,"[Prism probe] volume=%d H=%.0f/%.0f V=%.0f/%.0f center=%.0f/%.0f\n",int(volume),hx,hy,vx,vy,probe(one,0,0),probe(horizontal,0,0));
        CHECK(hx>hy+10);CHECK(vy>vx+10);
        if(!volume)CHECK(probe(horizontal,0,0)-probe(baseline,0,0)<(probe(one,0,0)-probe(baseline,0,0))*.5);
        p.prismCount=1;p.prismAngle=p.prismRotation=0;
    }
    p.light.coneCosInner=std::cos(.25f);p.light.coneCosOuter=std::cos(.3f);p.apertureRadius=.15f;p.pattern=LightPattern::Texture;p.focusDistance=1.9f;
    std::array<uint8_t,32*32*4> edge{};for(int y=0;y<32;++y)for(int x=0;x<32;++x){const auto i=(y*32+x)*4;edge[i]=edge[i+1]=edge[i+2]=x<16?255:0;edge[i+3]=255;}
    auto texture=f.renderer.createTextureFromRgba8(32,32,edge.data());CHECK(texture.isValid());p.goboTexture=texture;
    const auto sharp=f.sampleLight(p,false);p.focusDistance=.25f;const auto blurred=f.sampleLight(p,false);
    const auto variation=[](const auto& pixels){double sum=0;for(int y=29;y<=34;++y)for(int x=23;x<41;++x)sum=std::max(sum,double(std::abs(int(pixels[(y*64+x)*4])-int(pixels[(y*64+x+1)*4]))));return sum;};
    const auto a=variation(sharp),b=variation(blurred);std::fprintf(stderr,"[Focus probe] sharp=%.0f defocused=%.0f\n",a,b);CHECK(a>10);CHECK(b<a*.95);f.renderer.destroyTexture(texture);
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_CASE(camera_glare_uses_visible_hdr_and_zero_bloom_without_occlusion_leaks){
#ifdef _WIN32
    using namespace ayt::render;
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device camera glare acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    LensEffectsSettings lens;lens.enabled=true;lens.starStrength=2;lens.starAxes=1;lens.starLength=.2f;lens.ghostStrength=0;
    const auto baseline=f.sample(0),horizontal=f.sample(0,false,0,lens);lens.starRotation=1.570796327f;const auto vertical=f.sample(0,false,0,lens);
    const auto region=[](const auto& pixels,int x0,int y0,int x1,int y1){double sum=0;for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)sum+=pixels[(y*64+x)*4];return sum;};
    const auto h=region(horizontal,43,29,62,35),v=region(vertical,43,29,62,35),none=region(baseline,43,29,62,35);
    std::fprintf(stderr,"[Lens probe] horizontal=%.0f vertical=%.0f disabled=%.0f\n",h,v,none);CHECK(h>none+20);CHECK(h>v+20);
    lens.starStrength=0;lens.ghostStrength=1;lens.dispersion=.01f;
    const auto source=f.sample(5),ghost=f.sample(5,false,0,lens);CHECK(region(ghost,0,0,28,64)>region(source,0,0,28,64)+20);
    lens.starStrength=2;const auto blocked=f.sample(3,false,0,lens);CHECK(emitterRedEnergy(blocked)<emitterRedEnergy(ghost)*.02);
    lens.enabled=false;const auto disabled=f.sample(0,false,0,lens);CHECK(disabled==baseline);
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_CASE(opaque_fixture_hood_and_hinged_leaves_occlude_the_real_exit){
#ifdef _WIN32
    using namespace ayt::render;
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device fixture housing acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    const auto front=f.sample(0),frontHousing=f.sample(0,false,0,{},true),frontDark=f.sample(0,false,0,{},true,{},true);
    const double direct=emitterRedEnergy(front),frontExcess=emitterRedEnergy(frontHousing)-emitterRedEnergy(frontDark);
    CHECK(frontExcess>direct*.8);CHECK(f.housings.stats().fixtures==1);CHECK(f.housings.stats().parts==2);
    const auto side=f.sample(1),sideHousing=f.sample(1,false,0,{},true),sideDark=f.sample(1,false,0,{},true,{},true);
    const double sideExcess=emitterRedEnergy(sideHousing)-emitterRedEnergy(sideDark);
    CHECK(sideExcess<emitterRedEnergy(side)*.2);
    FixtureHousingSettings leaves;leaves.shadeLeaves=true;leaves.leafAngle=.5f;
    const auto shaded=f.sample(0,false,0,{},true,leaves),dark=f.sample(0,false,0,{},true,leaves,true);
    const double shadedExcess=emitterRedEnergy(shaded)-emitterRedEnergy(dark);
    std::fprintf(stderr,"[Housing probe] front=%.0f hood=%.0f oblique=%.0f/%.0f leaves=%.0f\n",direct,frontExcess,emitterRedEnergy(side),sideExcess,shadedExcess);
    CHECK(shadedExcess<frontExcess*.99);CHECK(shadedExcess>frontExcess*.1);CHECK(f.housings.stats().parts==6);
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_CASE(area_shadow_penumbra_grows_away_from_contact_and_softens_volume){
#ifdef _WIN32
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device PCSS acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    const auto partial=[&](const auto& pixels,const auto& lit,const auto& dark,float t){
        ayt::math::Float4x4 view,projection;CHECK(f.renderer.mainCameraMatrices(view,projection));
        const auto p=projection*view*ayt::math::FVector4(-1+t,0,-2+1.95f*t,1);const float cx=(p.x/p.w*.5f+.5f)*64,cy=(.5f-p.y/p.w*.5f)*64;
        const float half=std::abs(projection(0,0))*.14f/p.w*32+2;int count=0;
        for(int y=12;y<52;++y)for(int x=12;x<52;++x){if(std::abs(x-cx)<=half&&std::abs(y-cy)<=half)continue;
            const auto i=(y*64+x)*4;const float signal=float(lit[i])-dark[i];if(signal<15)continue;
            const float fraction=(float(pixels[i])-dark[i])/signal;if(fraction>.08f&&fraction<.92f)++count;
        }return count;
    };
    const auto nearLit=f.sampleShadow(.95f,.2f,false,false),nearDark=f.sampleShadow(.95f,.2f,false,false,false,true),nearSoft=f.sampleShadow(.95f,.2f,true,true);
    const auto farLit=f.sampleShadow(.5f,.2f,false,false),farDark=f.sampleShadow(.5f,.2f,false,false,false,true),farHard=f.sampleShadow(.5f,.2f,false,true),farSoft=f.sampleShadow(.5f,.2f,true,true);
    const int contact=partial(nearSoft,nearLit,nearDark,.95f),hard=partial(farHard,farLit,farDark,.5f),soft=partial(farSoft,farLit,farDark,.5f);
    std::fprintf(stderr,"[PCSS probe] contact=%d hard=%d separated=%d\n",contact,hard,soft);
    CHECK(soft>hard+5);CHECK(soft>contact+5);
    const auto smallLit=f.sampleShadow(.5f,.05f,false,false),smallDark=f.sampleShadow(.5f,.05f,false,false,false,true),smallSoft=f.sampleShadow(.5f,.05f,true,true);
    const int smallPenumbraPixels=partial(smallSoft,smallLit,smallDark,.5f);std::fprintf(stderr,"[PCSS aperture] small=%d large=%d\n",smallPenumbraPixels,soft);CHECK(soft>smallPenumbraPixels+5);
    const auto clearFog=f.sampleShadow(.5f,.2f,false,false,true),hardFog=f.sampleShadow(.5f,.2f,false,true,true),softFog=f.sampleShadow(.5f,.2f,true,true,true);
    double difference=0;for(size_t i=0;i<softFog.size();i+=4)difference+=std::abs(int(softFog[i])-int(hardFog[i]));
    std::fprintf(stderr,"[PCSS fog] clear=%.0f hard=%.0f soft=%.0f difference=%.0f\n",emitterRedEnergy(clearFog),emitterRedEnergy(hardFog),emitterRedEnergy(softFog),difference);
    CHECK(difference>10);CHECK(emitterRedEnergy(softFog)<=emitterRedEnergy(clearFog)+100);
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_CASE(transparent_pbr_receives_optical_lights_with_correct_alpha_layers_and_near_quotas){
#ifdef _WIN32
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device transparent lighting acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    const auto center=[](const auto& p){return double(p[(32*64+32)*4]);};
    const auto dark=f.sampleTransparent(.5f,0),nearPanePixels=f.sampleTransparent(.5f,6);CHECK(f.lastTransparentStats.transparentDraws==1);
    const double n=center(nearPanePixels)-center(dark);CHECK(n>20);
    const auto quarterDark=f.sampleTransparent(.25f,0),quarter=f.sampleTransparent(.25f,6);const double q=center(quarter)-center(quarterDark);CHECK(std::abs(n-2*q)<5);
    const auto rearDark=f.sampleTransparent(.5f,0,128,1),rearPanePixels=f.sampleTransparent(.5f,6,128,1);const double r=center(rearPanePixels)-center(rearDark);
    const auto stackDark=f.sampleTransparent(.5f,0,128,0,true),stack=f.sampleTransparent(.5f,6,128,0,true);CHECK(f.lastTransparentStats.transparentDraws==2);
    const double layered=center(stack)-center(stackDark);std::fprintf(stderr,"[Transparent probe] near=%.0f quarter=%.0f rear=%.0f layered=%.0f expected=%.0f\n",n,q,r,layered,n+r*.5);CHECK(std::abs(layered-(n+r*.5))<6);
    const auto limited=f.sampleTransparent(.5f,6,1,0,true);CHECK(f.lastTransparentStats.transparentDraws==1);CHECK(f.lastTransparentStats.transparentBudgetSkipped==1);CHECK(std::abs(center(limited)-center(stackDark)-n)<5);
    const auto disabled=f.sampleTransparent(.5f,6,0);CHECK(f.lastTransparentStats.transparentDraws==0);CHECK(disabled==dark);
    const auto blocked=f.sampleTransparent(.5f,6,128,0,false,true),opaqueOnly=f.sampleTransparent(0,6,128,0,false,true);CHECK(blocked==opaqueOnly);
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_CASE(transparent_pbr_uses_shared_gobo_and_ies_distribution){
#ifdef _WIN32
    using namespace ayt::render;
    const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Requires isolated real-device transparent optics acceptance");
    FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
    const auto dark=f.sampleTransparent(.5f,0),open=f.sampleTransparent(.5f,6),stripes=f.sampleTransparent(.5f,6,128,0,false,false,LightPattern::Stripes);
    const auto signal=[&](const auto& p){return emitterRedEnergy(p)-emitterRedEnergy(dark);};CHECK(signal(stripes)>signal(open)*.2);CHECK(signal(stripes)<signal(open)*.8);
    auto ies=std::make_shared<IesProfile>();CHECK(IesProfile::parse("IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 3 2 1 2 0 0 0\n1 1 1\n0 45 90\n0 180\n100 100 100\n0 0 0\n",*ies));
    const auto photometric=f.sampleTransparent(.5f,6,128,0,false,false,LightPattern::Open,ies);
    double left=0,right=0;for(int y=20;y<44;++y)for(int x=18;x<46;++x){const auto i=(y*64+x)*4;const double v=double(photometric[i])-dark[i];if(x<32)left+=v;else right+=v;}
    std::fprintf(stderr,"[Transparent optics] open=%.0f stripes=%.0f IES=%.0f/%.0f\n",signal(open),signal(stripes),left,right);CHECK(right>left*1.2);CHECK(signal(photometric)<signal(open));
#else
    SKIP_TEST("Windows real-device acceptance required");
#endif
}
TEST_SUITE_END
}

TEST_SUITE(ReducedVolumeGpu)
TEST_CASE(real_main_view_scattering_reconstruction_is_bounded_and_conserves_energy) {
#ifdef _WIN32
 const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Isolated real-device acceptance required");
 FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
 ayt::render::PerformanceLight lamp;lamp.id=1;lamp.light=ayt::render::Light::spot({-.5f,0,-2},{0,0,1},10,4,.95f,.8f,{1,0,0});lamp.apertureRadius=.1f;
 const auto full=f.sampleLight(lamp,true,1),half=f.sampleLight(lamp,true,2),quarter=f.sampleLight(lamp,true,4);
 const auto energy=emitterRedEnergy(full),halfEnergy=emitterRedEnergy(half),quarterEnergy=emitterRedEnergy(quarter);
 std::fprintf(stderr,"[Reduced volume] full=%.0f half=%.0f quarter=%.0f\n",energy,halfEnergy,quarterEnergy);
 CHECK(energy>1000);CHECK(halfEnergy>energy*.8);CHECK(halfEnergy<energy*1.15);CHECK(quarterEnergy>energy*.7);CHECK(quarterEnergy<energy*1.2);
#else
 SKIP_TEST("Windows device fixture required");
#endif
}
TEST_SUITE_END

TEST_SUITE(FroxelVolumeGpu)
TEST_CASE(real_main_view_grid_preserves_optics_and_local_medium_with_bounded_allocation) {
#ifdef _WIN32
 const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Isolated real-device acceptance required");
 FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
 using namespace ayt::render;
 PerformanceLight lamp;lamp.id=1;lamp.light=Light::spot({-.5f,0,-2},{0,0,1},10,4,.95f,.8f,{1,0,0});lamp.apertureRadius=.1f;
 VolumetricSettings fog;fog.enabled=true;fog.density=.1f;fog.scattering=1;fog.anisotropy=0;fog.steps=64;fog.debugView=2;fog.maxDistance=10;fog.froxelTileSize=4;fog.froxelSlices=64;fog.froxelMaxCells=65536;
 for(int mode=0;mode<3;++mode){
  if(mode==1){fog.local=true;fog.center={-.3f,0,-1};fog.halfExtent={2,2,2};fog.noiseStrength=.4f;fog.noiseTime=3;}
  if(mode==2){lamp.prismCount=2;lamp.prismAngle=.15f;lamp.pattern=LightPattern::Stripes;}
  fog.froxel=false;const auto ray=f.sampleLight(lamp,true,1,&fog);
  fog.froxel=true;const auto grid=f.sampleLight(lamp,true,1,&fog);const auto stats=f.renderer.getFrameStats().volumetric;
  const auto reference=emitterRedEnergy(ray),actual=emitterRedEnergy(grid);
  std::fprintf(stderr,"[Froxel main] mode=%d ray=%.0f grid=%.0f cells=%u\n",mode,reference,actual,stats.froxelCells);
  CHECK(reference>500);CHECK(actual>reference*.65);CHECK(actual<reference*1.35);
  CHECK(stats.froxel);CHECK(stats.depthSlices==64);CHECK(stats.froxelCells>0);CHECK(stats.froxelCells<=fog.froxelMaxCells);
 }
#else
 SKIP_TEST("Windows device fixture required");
#endif
}
TEST_SUITE_END

TEST_SUITE(FroxelHistoryGpu)
TEST_CASE(main_volume_history_is_independent_stable_and_dark_on_flash_off) {
#ifdef _WIN32
 const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Isolated real-device acceptance required");
 FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
 using namespace ayt::render;
 PerformanceLight lamp;lamp.id=1;lamp.light=Light::spot({-.5f,0,-2},{0,0,1},10,4,.95f,.8f,{1,0,0});lamp.apertureRadius=.1f;
 VolumetricSettings fog;fog.enabled=true;fog.froxel=true;fog.froxelTileSize=4;fog.froxelSlices=32;fog.maxDistance=10;fog.density=.1f;fog.scattering=1;fog.anisotropy=0;fog.debugView=2;
 const auto original=f.sampleLight(lamp,true,1,&fog);CHECK(!f.renderer.getFrameStats().volumetric.historyUsed);
 fog.froxelHistory=true;const auto stable=f.sampleLight(lamp,true,1,&fog);CHECK(f.renderer.getFrameStats().volumetric.historyUsed);
 unsigned difference=0;for(size_t i=0;i<original.size();++i)difference+=unsigned(std::abs(int(original[i])-int(stable[i])));CHECK(difference<64);
 f.renderer.resetVolumetricHistory();fog.froxelSlices=64;const auto resized=f.sampleLight(lamp,true,1,&fog);CHECK(f.renderer.getFrameStats().volumetric.depthSlices==64);CHECK(f.renderer.getFrameStats().volumetric.historyUsed);CHECK(emitterRedEnergy(resized)>1000);
 lamp.light.intensity=0;const auto dark=f.sampleLight(lamp,true,1,&fog);CHECK(emitterRedEnergy(dark)==0);
 fog.froxelHistory=false;f.sampleLight(lamp,true,1,&fog);CHECK(!f.renderer.getFrameStats().volumetric.historyUsed);
 std::fprintf(stderr,"[Froxel history] identity difference=%u flash-off=%.0f\n",difference,emitterRedEnergy(dark));
#else
 SKIP_TEST("Windows device fixture required");
#endif
}
TEST_SUITE_END

TEST_SUITE(FixtureHousingBatchGpu)
TEST_CASE(grouped_housings_instance_without_changing_opaque_pixels) {
#ifdef _WIN32
 const auto* backend=std::getenv("AY_EMITTER_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Isolated real-device acceptance required");
 FixtureEmitterGpuFixture f;const bool initialized=f.initialize(backend);CHECK(initialized);if(!initialized)return;
 f.renderer.setDebugOverlayEnabled(true);f.renderer.setDebugOverlaySuppressed(true);
 f.renderer.setAmbientStrength(1);f.renderer.setPostProcessExposure(8);f.renderer.setPostProcessBloomStrength(0);
 std::vector<ayt::render::PerformanceLight> lights;
 for(int i=0;i<16;++i){ayt::render::PerformanceLight p;p.id=uint32_t(i+1);p.light=ayt::render::Light::spot({(i%4-1.5f)*.4f,(i/4-1.5f)*.4f,0},ayt::math::FVector3((i%4-1.5f)*.1f,(i/4-1.5f)*.1f,-1).normalize(),10,0,.95f,.8f,{1,1,1});p.apertureRadius=.1f;lights.push_back(p);}
 const auto render=[&](bool instanced){
  f.renderer.setRigidInstancingEnabled(instanced);ayt::render::FixtureHousingSettings settings;settings.shadeLeaves=true;settings.leafAngle=.2f;
  ayt::render::RenderScene scene;CHECK(f.housings.append(f.renderer,scene,lights,settings));CHECK(scene.items().size()==96);
  for(int i=0;i<6;++i){f.renderer.beginFrame({0,0,0,1});f.renderer.render(scene);f.renderer.endFrame();}
  const auto stats=f.renderer.getFrameStats();const auto color=bgfx::TextureHandle{ayt::render::detail::CameraViewsTestAccess::finalColor(f.renderer)};CHECK(bgfx::isValid(color));std::array<uint8_t,64*64*4> pixels{};
  if(bgfx::isValid(color)){bgfx::blit(112,f.readback,0,0,color);const auto ready=bgfx::readTexture(f.readback,pixels.data());auto* adapter=ayt::render::detail::CameraViewsTestAccess::adapter(f.renderer);for(int i=0;i<120&&adapter->gpuFrameCounter()<ready;++i)f.renderer.endFrame();CHECK(adapter->gpuFrameCounter()>=ready);}
  return std::pair{pixels,stats};
 };
 const auto [individual,baseline]=render(false);const auto [batched,optimized]=render(true);
 const double energy=emitterRedEnergy(individual);double difference=0;for(size_t i=0;i<individual.size();i+=4)for(size_t c=0;c<3;++c)difference+=std::abs(int(individual[i+c])-int(batched[i+c]));
 std::fprintf(stderr,"[Housing batch] individual=%u grouped=%u objects=%u energy=%.0f difference=%.0f\n",baseline.backendDrawCalls,optimized.backendDrawCalls,optimized.instancedObjects,energy,difference);
 double viewGpu=0;for(const auto& pass:optimized.passes)viewGpu+=pass.gpuTimeMs;
 CHECK(viewGpu>0);CHECK(optimized.gpuFrameNumber>0);
 CHECK(energy>500);CHECK(baseline.instancedDrawCalls==0);CHECK(optimized.instancedDrawCalls>=3);CHECK(optimized.instancedObjects>=96);CHECK(optimized.backendDrawCalls<baseline.backendDrawCalls/2);CHECK(difference<energy*.02+30);
#else
 SKIP_TEST("Windows device fixture required");
#endif
}
TEST_SUITE_END
