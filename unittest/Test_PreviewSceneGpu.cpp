#include <AYRenderer.h>
#include <AYRenderer/UIRenderBackend.h>
#include <AYTest.h>
#include <bgfx/bgfx.h>
#include "detail/TextureImageLoader.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

TEST_SUITE(AYRenderer_PreviewSceneGpu)
TEST_CASE(multiple_hdr_previews_keep_display_state_and_survive_resize_release_with_main_post)
{
#ifdef _WIN32
    const auto *backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !*backend)
        SKIP_TEST("Isolated D3D11/12 device required");
    using namespace ayt::render;
    using ayt::ui::IRenderBackend;
    auto window = CreateWindowExW(0, L"STATIC", L"Preview display regression", WS_OVERLAPPED, 0, 0, 256, 144,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(window != nullptr);
    if (!window)
        return;
    struct WindowCleanup
    {
        HWND window;
        ~WindowCleanup()
        {
            DestroyWindow(window);
        }
    } cleanup{window};
    Renderer renderer;
    InitDesc init;
    init.windowHandle = window;
    init.width = 240;
    init.height = 120;
    init.msaa = 0;
    init.vsync = false;
    init.backend = std::string(backend) == "d3d12" ? Backend::Direct3D12 : Backend::Direct3D11;
    CHECK(renderer.initialize(init));
    if (!renderer.isInitialized())
        return;
    renderer.setFxaaEnabled(false);
    renderer.setSmaaEnabled(false);
    renderer.setTaaEnabled(false);
    renderer.setPostProcessGamma(1);
    renderer.setPostProcessExposure(1);
    renderer.setPostProcessTonemapMode(Renderer::TonemapMode::None);
    UIRenderBackend ui;
    CHECK(ui.initialize(renderer));
    ui.setFramebufferSize(240, 120);
    std::array<IRenderBackend::RenderTargetHandle, 3> targets;
    for (auto &target : targets)
    {
        target = ui.createRenderTarget(64, 48, false);
        CHECK(target.isValid());
    }
    std::array<PreviewSceneSettings, 3> settings;
    settings[1].displayTransform = true;
    settings[1].exposure = 1.7f;
    settings[1].gamma = 2.2f;
    settings[1].toneMap = PreviewToneMap::ACES;
    settings[2].displayTransform = true;
    settings[2].exposure = .65f;
    settings[2].gamma = 1.8f;
    settings[2].toneMap = PreviewToneMap::Reinhard;
    constexpr uint32_t colors[] = {0x3f7fbfff, 0x3f7fbfff, 0x20a060ff};
    PreviewSceneCamera camera;
    RenderScene scene;
    const auto originalCamera = renderer.mainCameraPosition();
    const auto *directory = std::getenv("AY_PBR_SURFACE_CAPTURE_DIRECTORY");
    const auto root = directory
                          ? std::filesystem::path(directory) / backend / "previews"
                          : std::filesystem::temp_directory_path() /
                                (std::string("ay-preview-gpu-") + backend + "-" +
                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    for (int mode = 0; mode < 4; ++mode)
    {
        const int width = mode ? 256 : 240, height = mode ? 144 : 120;
        if (mode == 1)
        {
            renderer.resize(uint16_t(width), uint16_t(height));
            ui.setFramebufferSize(uint16_t(width), uint16_t(height));
            IRenderBackend::RenderTargetDesc desc;
            desc.width = 48;
            desc.height = 32;
            for (auto target : targets)
                CHECK(ui.resizeRenderTarget(target, desc));
        }
        if (mode == 2)
        {
            ui.releaseRenderTarget(targets[1]);
            targets[1] = ui.createRenderTarget(32, 32, false);
            CHECK(targets[1].isValid());
        }
        renderer.configurePipeline(mode >= 2 ? RenderPipelineDesc::makeDeferred()
                                             : RenderPipelineDesc::makeDefault());
        renderer.setShadowsEnabled(false);
        const auto path = root / ("display-" + std::to_string(mode) + ".png");
        for (int frame = 0; frame < 14; ++frame)
        {
            ClearDesc clear;
            clear.r = .11f;
            clear.g = .23f;
            clear.b = .37f;
            renderer.beginCompositeFrame(clear, uint16_t(width), uint16_t(height));
            renderer.render(scene);
            ui.beginFrame();
            ui.beginCanvas({0, 0, float(width), float(height)});
            int callbacks = 0;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                CHECK(ui.renderScenePreview(targets[i], scene, camera, colors[i], settings[i],
                                            [&](uint16_t) { ++callbacks; }));
                const float x = 8.f + 72.f * i;
                ui.blitRenderTarget(targets[i], {x, 8, x + 64, 56});
            }
            CHECK(callbacks == 3);
            auto invalid = camera;
            invalid.position.z = std::numeric_limits<float>::quiet_NaN();
            CHECK_FALSE(ui.renderScenePreview(targets[0], scene, invalid, 0, settings[1],
                                              [&](uint16_t) { ++callbacks; }));
            CHECK(callbacks == 3);
            ui.endCanvas();
            ui.endFrame();
            if (frame == 10)
                CHECK(renderer.captureScreenshot(path.string()));
            renderer.endFrame();
        }
        const auto image = detail::decodeImageFile(path.string());
        CHECK(image.width == width);
        CHECK(image.height == height);
        if (image.rgba8.size() != size_t(width * height * 4))
            continue;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            int mismatches = 0;
            for (int c = 0; c < 4; ++c)
            {
                const auto encoded = (colors[i] >> (24 - c * 8)) & 255u;
                float value = float(encoded) / 255;
                if (c < 3 && settings[i].displayTransform)
                {
                    value *= settings[i].exposure;
                    value = settings[i].toneMap == PreviewToneMap::ACES
                                ? value * (2.51f * value + .03f) / (value * (2.43f * value + .59f) + .14f)
                                : value / (1 + value);
                    value = std::pow(value, 1 / settings[i].gamma);
                }
                const int expected = int(std::lround(std::clamp(value, 0.f, 1.f) * 255));
                for (int y = 12; y < 52; ++y)
                    for (int x = 12 + int(i) * 72; x < 68 + int(i) * 72; ++x)
                        if (std::abs(int(image.rgba8[(y * width + x) * 4 + c]) - expected) > 1)
                            ++mismatches;
            }
            std::cout << "[PreviewDisplay] backend=" << backend << " mode=" << mode << " preview=" << i
                      << " mismatches=" << mismatches << '\n';
            CHECK(mismatches == 0);
        }
        // The main view's display settings must not inherit either preview.
        const int mainPixel = ((height - 10) * width + width - 10) * 4;
        const int expectedMain[] = {int(.11f * 255), int(.23f * 255), int(.37f * 255), 255};
        for (int c = 0; c < 4; ++c)
            CHECK(std::abs(int(image.rgba8[mainPixel + c]) - expectedMain[c]) <= 1);
        CHECK(ui.getLayerCacheStats().liveTargetLeases == 5);
    }
    const auto after = renderer.mainCameraPosition();
    CHECK(after.x == originalCamera.x && after.y == originalCamera.y && after.z == originalCamera.z);
    for (auto target : targets)
        ui.releaseRenderTarget(target);
    CHECK(ui.getLayerCacheStats().liveTargetLeases == 0);
    ui.shutdown();
    renderer.shutdown();
#else
    SKIP_TEST("Windows D3D device required");
#endif
}
TEST_CASE(wrapped_ui_paragraphs_match_independent_line_draws_across_scale_alignment_and_clipping)
{
#ifdef _WIN32
    const auto* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend||!*backend)SKIP_TEST("Isolated D3D11/12 device required");
    using namespace ayt::render;
    using Style=ayt::ui::IRenderBackend::TextStyle;
    auto window=CreateWindowExW(0,L"STATIC",L"Wrapped paragraph regression",WS_OVERLAPPED,0,0,1024,768,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);if(!window)return;
    struct CloseWindow{HWND value;~CloseWindow(){DestroyWindow(value);}} close{window};
    Renderer renderer;InitDesc init;init.windowHandle=window;init.width=512;init.height=384;
    init.msaa=0;init.vsync=false;
    init.backend=std::string(backend)=="d3d12"?Backend::Direct3D12:Backend::Direct3D11;
    CHECK(renderer.initialize(init));if(!renderer.isInitialized())return;
    renderer.setFxaaEnabled(false);renderer.setSmaaEnabled(false);renderer.setTaaEnabled(false);
    renderer.setPostProcessGamma(1);renderer.setPostProcessExposure(1);
    renderer.setPostProcessTonemapMode(Renderer::TonemapMode::None);
    UIRenderBackend ui;CHECK(ui.initialize(renderer));
    const auto* evidence=std::getenv("AY_UI_PARAGRAPH_CAPTURE_DIRECTORY");
    const auto directory=evidence?std::filesystem::path(evidence)/backend:
        std::filesystem::temp_directory_path()/(std::string("ay-ui-paragraph-")+backend+"-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const std::wstring combined=L"\nAlpha\r\n\rBeta\u2028中文\u2029العربية\n";
    const std::array<std::wstring,7> reference={L"",L"Alpha",L"",L"Beta",L"中文",L"العربية",L""};
    const float scales[]={1,1.25f,2};
    for(unsigned mode=0;mode<3;++mode){
        const float scale=scales[mode];const uint16_t width=uint16_t(512*scale),height=uint16_t(384*scale);
        renderer.resize(width,height);ui.setFramebufferSize(width,height);ui.setUiScale(scale);
        Style style;style.wrapToBounds=true;style.valign=static_cast<Style::VAlign>(mode);
        style.align=static_cast<Style::Align>(mode);style.lineSpacing=mode?4:0;
        style.color={.9f,.95f,1,1};
        if(mode==2){style.outlineWidth=1;style.outlineColor={.1f,.3f,.5f,1};
            style.shadowOffset={1,2};style.shadowColor={.8f,.1f,.15f,1};}
        constexpr int fontSize=22;
        const auto one=ui.measureText(L"M",fontSize,style);CHECK(one.height>0);
        // Independent reference: the known seven logical lines and explicit
        // block alignment; the actual paragraph layout/measure API is unused.
        const float block=one.height*reference.size()+style.lineSpacing*(reference.size()-1);
        const float unused=std::max(0.f,340-block);
        const float startY=20+(style.valign==Style::VAlign::Top?0:
            style.valign==Style::VAlign::Middle?unused*.5f:unused);
        const auto path=directory/("paragraph-"+std::to_string(mode)+".png");
        for(int frame=0;frame<18;++frame){
            ClearDesc clear;clear.r=.025f;clear.g=.035f;clear.b=.05f;
            renderer.beginCompositeFrame(clear,width,height);renderer.render(RenderScene{});
            ui.beginFrame();ui.beginCanvas({0,0,512,384});
            if(mode==2)ui.pushClip({0,16,512,320});
            ui.drawText({8,20,248,360},combined,fontSize,style);
            auto manual=style;manual.wrapToBounds=false;manual.valign=Style::VAlign::Top;
            for(size_t line=0;line<reference.size();++line){
                const float y=startY+line*(one.height+style.lineSpacing);
                ui.drawText({264,y,504,y+one.height},reference[line],fontSize,manual);
            }
            if(mode==2)ui.popClip();
            ui.endCanvas();ui.endFrame();
            if(frame==12)CHECK(renderer.captureScreenshot(path.string()));
            renderer.endFrame();
        }
        const auto image=detail::decodeImageFile(path.string());CHECK(image.width==width);CHECK(image.height==height);
        if(image.rgba8.size()!=size_t(width)*height*4)continue;
        const unsigned half=width/2;size_t differences=0,lit=0;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<half;++x){
            const auto left=(size_t(y)*width+x)*4,right=(size_t(y)*width+x+half)*4;
            bool differs=false;
            for(unsigned c=0;c<4;++c)differs|=std::abs(int(image.rgba8[left+c])-int(image.rgba8[right+c]))>1;
            differences+=differs;lit+=image.rgba8[right]>80&&image.rgba8[right+1]>80;
        }
        std::cout<<"[UiParagraph] backend="<<backend<<" scale="<<scale<<" differences="<<differences
            <<" reference pixels="<<lit<<'\n';
        CHECK(lit>100);CHECK(differences==0);
    }
    ui.shutdown();renderer.shutdown();
#else
    SKIP_TEST("Windows D3D device required");
#endif
}
// design reference: design.md, UI program ownership and backend shutdown.
TEST_CASE(closing_one_ui_backend_keeps_other_same_pool_flat_and_sdf_pixels_alive)
{
#ifdef _WIN32
    const auto* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend||!*backend)SKIP_TEST("Isolated D3D11/12 device required");
    using namespace ayt::render;
    constexpr uint16_t width=176,height=64;
    const auto window=CreateWindowExW(0,L"STATIC",L"Shared UI pool lifecycle",WS_OVERLAPPED,
        0,0,256,144,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window!=nullptr);if(!window)return;
    struct CloseWindow{HWND value;~CloseWindow(){DestroyWindow(value);}} close{window};
    Renderer renderer;InitDesc init;init.windowHandle=window;init.width=width;init.height=height;
    init.msaa=0;init.vsync=false;
    init.backend=std::string(backend)=="d3d12"?Backend::Direct3D12:Backend::Direct3D11;
    CHECK(renderer.initialize(init));if(!renderer.isInitialized())return;
    renderer.setFxaaEnabled(false);renderer.setSmaaEnabled(false);renderer.setTaaEnabled(false);
    renderer.setPostProcessGamma(1);renderer.setPostProcessExposure(1);
    renderer.setPostProcessTonemapMode(Renderer::TonemapMode::None);
    UIRenderBackend survivor,visitor;
    CHECK(survivor.initialize(renderer));if(!survivor.isInitialized())return;
    survivor.setFramebufferSize(width,height);
    const auto* output=std::getenv("AY_UI_SHARED_POOL_CAPTURE_DIRECTORY");
    const auto root=output?std::filesystem::path(output)/backend:
        std::filesystem::temp_directory_path()/(std::string("ay-ui-shared-pool-")+backend+"-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto capture=[&](UIRenderBackend& ui,const std::string& name){
        const auto path=root/(name+".png");
        for(int frame=0;frame<18;++frame){
            ClearDesc clear;clear.r=.025f;clear.g=.035f;clear.b=.05f;
            renderer.beginCompositeFrame(clear,width,height);renderer.render(RenderScene{});
            ui.beginFrame();ui.beginCanvas({0,0,float(width),float(height)});
            ui.drawRect({8,8,80,56},{.8f,.2f,.1f,1});
            ui.drawRoundedRect({88,8,160,56},{.1f,.7f,.3f,1},8);
            ui.endCanvas();ui.endFrame();
            if(frame==12)CHECK(renderer.captureScreenshot(path.string()));
            renderer.endFrame();
        }
        CHECK(ui.getDrawCallCount()==2);
        const auto image=detail::decodeImageFile(path.string());
        CHECK(image.width==width);CHECK(image.height==height);
        if(image.rgba8.size()!=size_t(width)*height*4)return;
        const std::array<std::array<int,4>,2> expected={{{204,51,26,255},{26,179,77,255}}};
        for(int region=0;region<2;++region){
            size_t mismatches=0;
            for(unsigned y=20;y<44;++y)for(unsigned x=20+region*80;x<68+region*80;++x){
                const auto offset=(size_t(y)*width+x)*4;
                for(unsigned channel=0;channel<4;++channel)
                    mismatches+=std::abs(int(image.rgba8[offset+channel])-expected[region][channel])>1;
            }
            std::cout<<"[SharedUiPool] backend="<<backend<<" phase="<<name<<" region="<<region
                <<" mismatches="<<mismatches<<'\n';
            CHECK(mismatches==0);
        }
    };
    capture(survivor,"survivor-only");
    const auto warmPrograms=bgfx::getStats()->numPrograms;
    const auto warmTextures=bgfx::getStats()->numTextures;
    CHECK(visitor.initialize(renderer));if(!visitor.isInitialized())return;
    visitor.setFramebufferSize(width,height);
    capture(survivor,"both-initialized");
    CHECK(bgfx::getStats()->numPrograms==warmPrograms);
    visitor.shutdown();
    CHECK(!visitor.isInitialized());CHECK(survivor.isInitialized());
    capture(survivor,"visitor-closed");
    CHECK(bgfx::getStats()->numPrograms==warmPrograms);
    CHECK(bgfx::getStats()->numTextures==warmTextures);
    CHECK(visitor.initialize(renderer));if(!visitor.isInitialized())return;
    visitor.shutdown();
    capture(survivor,"visitor-reopened-and-closed");
    CHECK(bgfx::getStats()->numPrograms==warmPrograms);
    CHECK(bgfx::getStats()->numTextures==warmTextures);
    // Reverse the roles too: a new backend must survive closing the original.
    CHECK(visitor.initialize(renderer));if(!visitor.isInitialized())return;
    visitor.setFramebufferSize(width,height);
    survivor.shutdown();
    capture(visitor,"original-survivor-closed");
    CHECK(survivor.initialize(renderer));if(!survivor.isInitialized())return;
    survivor.setFramebufferSize(width,height);
    capture(survivor,"survivor-rebuilt");
    visitor.shutdown();
    capture(survivor,"rebuilt-survivor-after-visitor-close");
    CHECK(bgfx::getStats()->numPrograms==warmPrograms);
    CHECK(bgfx::getStats()->numTextures==warmTextures);
    survivor.shutdown();renderer.shutdown();
#else
    SKIP_TEST("Windows D3D device required");
#endif
}

TEST_SUITE_END
