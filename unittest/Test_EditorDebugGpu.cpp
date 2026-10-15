#include <AYTest.h>
#include <AYRenderer.h>
#include "detail/TextureImageLoader.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

TEST_SUITE(AYRenderer_PhysicsDebugGpu)
TEST_CASE(real_d3d11_overlay_pixels_show_both_colors_and_clear) {
#ifdef _WIN32
    if(!std::getenv("AY_PHYSICS_DEBUG_GPU_TEST")) SKIP_TEST("Requires isolated D3D11 device");
    HWND window=CreateWindowExW(0,L"STATIC",L"Physics debug acceptance",WS_OVERLAPPED,
        0,0,256,256,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window!=nullptr); if(!window) return;
    struct WindowGuard { HWND h; ~WindowGuard(){DestroyWindow(h);} } guard{window};
    ayt::render::Renderer renderer;
    ayt::render::InitDesc init; init.windowHandle=window; init.backend=ayt::render::Backend::Direct3D11;
    init.width=256; init.height=256; init.vsync=false; init.msaa=0;
    CHECK(renderer.initialize(init)); if(!renderer.isInitialized()) return;
    renderer.configurePipeline(ayt::render::RenderPipelineDesc::makeEditorForward());
    renderer.setMainCamera(ayt::math::Float4x4::identity(),ayt::math::Float4x4::identity());
    CHECK(renderer.setEditorDebugLines({
        {{-0.8f,-0.3f,0.5f},{0.8f,-0.3f,0.5f},0xff00ff00u},
        {{-0.8f,0.3f,0.5f},{0.8f,0.3f,0.5f},0xff0000ffu}}));
    const char* output=std::getenv("AY_PHYSICS_DEBUG_CAPTURE_DIRECTORY");
    const auto root=output?std::filesystem::path(output):std::filesystem::current_path();
    std::filesystem::create_directories(root);
    ayt::render::RenderScene scene;
    auto capture=[&](const char* name) {
        const auto path=(root/name).string();
        for(int frame=0;frame<8;++frame) {
            renderer.beginFrame(); renderer.render(scene);
            if(frame>=3) CHECK(renderer.captureScreenshot(path));
            renderer.endFrame();
        }
        auto image=ayt::render::detail::decodeImageFile(path);
        CHECK(image.width==256 && image.height==256); CHECK(!image.rgba8.empty());
        return image;
    };
    auto counts=[](const auto& image) {
        size_t red=0,green=0;
        for(size_t i=0;i+3<image.rgba8.size();i+=4) {
            if(image.rgba8[i]>220&&image.rgba8[i+1]<30&&image.rgba8[i+2]<30) ++red;
            if(image.rgba8[i+1]>220&&image.rgba8[i]<30&&image.rgba8[i+2]<30) ++green;
        }
        return std::pair{red,green};
    };
    const auto [red,green]=counts(capture("physics-debug-colors.png"));
    std::cerr<<"[Physics debug GPU] red="<<red<<" green="<<green<<'\n';
    CHECK(red>100); CHECK(green>100);
    const char* materialSource=R"(material DebugOccluder {
        vertex { in pos : position return modelViewProjection * vec4(pos, 1.0) }
        fragment { return vec4(0.0,0.0,0.8,1.0) }
    })";
    auto material=renderer.createMaterialFromPhoskia(materialSource,"debug_occluder");CHECK(material.isValid());
    auto mesh=renderer.createUnitCube();CHECK(mesh.isValid());
    ayt::render::DrawItem item;item.mesh=mesh;item.material=material;
    item.world=ayt::math::translate(0,0,.2f)*ayt::math::scale(.6f,1.2f,.2f);scene.add(item);
    auto dominant=[](const auto& image) {
        size_t count=0;for(size_t i=0;i+3<image.rgba8.size();i+=4) {
            const auto r=image.rgba8[i],g=image.rgba8[i+1],b=image.rgba8[i+2];
            if((r>80 && r>g*2 && r>b*2)||(g>80 && g>r*2 && g>b*2)) ++count;
        }return count;
    };
    const auto xray=dominant(capture("physics-debug-xray.png"));
    renderer.setEditorDebugDepthTest(true);
    const auto depth=dominant(capture("physics-debug-depth-forward.png"));
    std::cerr<<"[Physics debug depth GPU] xray="<<xray<<" visible="<<depth<<'\n';
    CHECK(depth>30 && depth+40<xray);
    renderer.configurePipeline(ayt::render::RenderPipelineDesc::makeEditorDeferred());
    const auto deferredDepth=dominant(capture("physics-debug-depth-deferred.png"));
    CHECK(deferredDepth>30 && deferredDepth+40<xray);
    CHECK(renderer.setEditorDebugLines({}));
    const auto cleared=counts(capture("physics-debug-cleared.png"));
    CHECK(cleared.first==0 && cleared.second==0);
    renderer.shutdown();
#else
    SKIP_TEST("Windows D3D11 fixture required");
#endif
}
TEST_SUITE_END
