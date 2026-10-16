#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <AYRenderer.h>
#include <AYRenderer/EmissivePointCloud.h>
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
