#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "AYTest.h"
#include "AYRenderer.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShaderBuild.h"
#include "AYShader/ShadercDriver.h"
#include "detail/BuiltinShaderCatalog.h"
#include "detail/ShaderProduction.h"
#include "detail/ShaderPoolSetup.h"
#include "AYShader/detail/ComputeResourceAccess.h"
#include <AYIO/File.h>
#include "detail/BGFXAdapter.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/GpuResources.h"
#include "AYRenderer/SsrShaderSources.h"
#include "AYRenderer/PbrShaderSources.h"
#include "AYRenderer/VolumetricShaderSources.h"
#include "AYRenderer/DeferredDecalShaderSources.h"
#include "detail/DeferredDecalPass.h"
#include "detail/ColorStylizationPass.h"
#include "detail/StylizedOutlinePass.h"
#include "detail/AnalogDisplayPass.h"
#include "detail/RenderViewOrder.h"
#include "detail/DeferredDecalVisibility.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/PassExecContext.h"
#include "detail/BgfxMatrix.h"
#include "detail/TextureImageLoader.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
using namespace ayt::render::detail;
using Pixel = std::array<float, 4>;

// Fresh-process real-device checks. Reuse the renderer GPU opt-in, never the
// portable suite's persistent Noop context, and never steal editor input.
struct PhoskiaGpuFixture {
    HWND window = nullptr;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    FullscreenPassGeometry geometry;
    std::vector<bgfx::TextureHandle> textures;
    bgfx::TextureHandle targetTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle readback = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE;
    static constexpr uint16_t size = 4;

    ~PhoskiaGpuFixture() {
        if (adapter.isInitialized()) {
            pool.shutdown();
            geometry.destroy(adapter);
            if (bgfx::isValid(target)) bgfx::destroy(target);
            for (auto texture : textures) bgfx::destroy(texture);
            adapter.shutdown();
        }
        if (window) DestroyWindow(window);
    }

    bool initialize(const char* backend) {
        window = CreateWindowExW(0, L"STATIC", L"Phoskia GPU regression", WS_OVERLAPPED,
            0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return false;
        BGFXInitParams init;
        init.nativeWindowHandle = window;
        init.width = init.height = 64;
        init.vsync = false;
        init.backend = std::string(backend) == "d3d12"
            ? ayt::render::Backend::Direct3D12 : ayt::render::Backend::Direct3D11;
        if (!adapter.initialize(init)) return false;
        const auto required = BGFX_CAPS_TEXTURE_READ_BACK | BGFX_CAPS_TEXTURE_BLIT;
        if ((bgfx::getCaps()->supported & required) != required) return false;
        pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
        pool.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT, AY_SHADER_BGFX_SRC_HINT});
        pool.resolvePlatformFromRenderer();
        if (!geometry.ensure(adapter)) return false;
        targetTexture = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RGBA32F,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (bgfx::isValid(targetTexture)) textures.push_back(targetTexture);
        readback = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RGBA32F,
            BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
        if (bgfx::isValid(readback)) textures.push_back(readback);
        if (!bgfx::isValid(targetTexture) || !bgfx::isValid(readback)) return false;
        target = bgfx::createFrameBuffer(1, &targetTexture, false);
        return bgfx::isValid(target);
    }

    bgfx::TextureHandle input(Pixel color) {
        auto texture = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
            BGFX_SAMPLER_POINT, bgfx::copy(color.data(), sizeof(color)));
        if (bgfx::isValid(texture)) textures.push_back(texture);
        return texture;
    }

    std::vector<Pixel> readImage(bgfx::TextureHandle image) {
        bgfx::blit(2,readback,0,0,image);
        std::vector<Pixel> pixels(size*size);
        const auto ready=bgfx::readTexture(readback,pixels.data());
        for(int frame=0;frame<120 && adapter.gpuFrameCounter()<ready;++frame) adapter.endFrame();
        CHECK(adapter.gpuFrameCounter()>=ready);
        return pixels;
    }

    std::vector<Pixel> render(const std::string& declarations, const std::string& body,
        const std::function<void(const ayt::shader::ShaderResource&)>& bind = {}) {
        const auto source = "material Probe { " + declarations +
            " vertex { in position : position; return vec4(position, 1.0) } fragment { " + body + " } }";
        return renderSource(source, bind);
    }

    std::vector<Pixel> renderSource(const std::string& source,
        const std::function<void(const ayt::shader::ShaderResource&)>& bind = {}) {
        auto program = pool.acquire(source);
        return renderResource(program, bind);
    }

    std::vector<Pixel> renderResource(const ayt::shader::ShaderResource& program,
        const std::function<void(const ayt::shader::ShaderResource&)>& bind = {}) {
        CHECK(program.isValid());
        if (!program.isValid()) {
            for (const auto& error : pool.lastCompileErrors()) std::cerr << error << '\n';
            return {};
        }
        configureFullscreenPassView(adapter, 0, target, 0, 0, size, size);
        geometry.bind(adapter);
        if (bind) bind(program);
        adapter.setStateDepthTestAlways();
        program.submit({0, 0});
        bgfx::blit(1, readback, 0, 0, targetTexture);
        std::vector<Pixel> pixels(size * size);
        const auto ready = bgfx::readTexture(readback, pixels.data());
        for (int frame = 0; frame < 120 && adapter.gpuFrameCounter() < ready; ++frame) adapter.endFrame();
        CHECK(adapter.gpuFrameCounter() >= ready);
        return pixels;
    }
};

void checkPixels(const std::vector<Pixel>& pixels, Pixel expected) {
    CHECK(pixels.size() == PhoskiaGpuFixture::size * PhoskiaGpuFixture::size);
    if (!pixels.empty()) {
        std::cerr << "[Phoskia GPU] expected=" << expected[0] << ',' << expected[1] << ',' << expected[2] << ',' << expected[3]
                  << " actual=" << pixels.front()[0] << ',' << pixels.front()[1] << ',' << pixels.front()[2] << ',' << pixels.front()[3] << '\n';
    }
    for (const auto& pixel : pixels)
        for (size_t channel = 0; channel < 4; ++channel)
            CHECK(std::isfinite(pixel[channel]) && std::abs(pixel[channel] - expected[channel]) < 0.0001f);
}
} // namespace
#endif

TEST_SUITE(AYRenderer_PhoskiaGpu)

TEST_CASE(forward_deferred_background_and_stylization_share_viewport_and_pbr) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0]) SKIP_TEST("Isolated D3D11/12 device required");
    HWND window=CreateWindowExW(0,L"STATIC",L"Main pipeline color parity",WS_OVERLAPPED,
        0,0,160,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);if(!window)return;
    struct WindowCleanup{HWND h;~WindowCleanup(){DestroyWindow(h);}} cleanup{window};
    ayt::render::Renderer renderer;ayt::render::InitDesc init;
    init.windowHandle=window;init.width=160;init.height=120;init.msaa=0;init.vsync=false;
    init.backend=std::string(backend)=="d3d12"?ayt::render::Backend::Direct3D12:ayt::render::Backend::Direct3D11;
    CHECK(renderer.initialize(init));if(!renderer.isInitialized())return;
    renderer.setFxaaEnabled(false);renderer.setSmaaEnabled(false);renderer.setTaaEnabled(false);
    renderer.setPostProcessGamma(1);renderer.setPostProcessExposure(1);
    renderer.setPostProcessTonemapMode(ayt::render::Renderer::TonemapMode::None);
    renderer.setDirectionalLight({0,0,1},{1.4f,1.2f,.9f});
    renderer.setMainCamera(ayt::math::Float4x4::identity(),ayt::math::Float4x4::identity(),{0,0,-2});
    const float vertices[][12]={
        {-.3f,-.3f,.5f,0,0,-1,1,0,0,1,0,1},{.3f,-.3f,.5f,0,0,-1,1,0,0,1,1,1},
        {.3f,.3f,.5f,0,0,-1,1,0,0,1,1,0},{-.3f,.3f,.5f,0,0,-1,1,0,0,1,0,0}};
    const uint16_t indices[]={0,2,1,0,3,2};
    using namespace ayt::render;
    VertexLayoutDesc layout;
    layout.add({VertexAttribute::Position,3,VertexComponentType::Float,false});
    layout.add({VertexAttribute::Normal,3,VertexComponentType::Float,false});
    layout.add({VertexAttribute::Tangent,4,VertexComponentType::Float,false});
    layout.add({VertexAttribute::TexCoord0,2,VertexComponentType::Float,false});
    auto mesh=renderer.createMesh(vertices,4,layout,indices,6);CHECK(mesh.isValid());
    auto material=renderer.createMaterialFromPhoskia(kPbrPhoskiaSource);
    CHECK(material.isValid());renderer.setMaterialColor(material,"baseColor",.2f,.4f,.6f,1);
    renderer.setMaterialFloat(material,"metallic",.7f);renderer.setMaterialFloat(material,"roughness",.65f);
    renderer.setMaterialFloat(material,"ao",1);renderer.setMaterialFloat(material,"opacity",1);
    renderer.setMaterialFloat(material,"normalYSign",1);renderer.setMaterialVec3(material,"emissive",0,0,0);
    for(const char* name:{"opacitySource","doubleSided","premultipliedAlpha","castSkinned"})renderer.setMaterialFloat(material,name,0);
    RenderScene scene;scene.add(mesh,material);
    const auto directory=std::getenv("AY_COLOR_PARITY_CAPTURE_DIRECTORY");
    const auto root=directory?std::filesystem::path(directory)/backend:std::filesystem::temp_directory_path()/(std::string("ay-color-parity-")+backend+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    std::array<decltype(decodeImageFile(std::string{})),4> forwardImages;
    const int bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for(bool deferred:{false,true})for(int mode=0;mode<4;++mode) {
        renderer.configurePipeline(deferred?RenderPipelineDesc::makeDeferred():RenderPipelineDesc::makeDefault());
        renderer.setShadowsEnabled(false);
        const bool composite=mode>=2,style=mode%2;
        const int ox=composite?13:0,oy=composite?9:0,w=composite?128:160,h=composite?96:120;
        renderer.setViewportRect(uint16_t(ox),uint16_t(oy),uint16_t(w),uint16_t(h));
        ClearDesc clear;clear.r=composite?.41f:.23f;clear.g=composite?.26f:.37f;clear.b=composite?.17f:.61f;
        auto settings=colorStylizationPreset(ColorStylizationPreset::DitherRetro);
        settings.enabled=style;settings.pixelationEnabled=false;renderer.setColorStylizationSettings(settings);
        const auto path=root/((deferred?std::string("deferred-"):std::string("forward-"))+std::to_string(mode)+".png");
        for(int frame=0;frame<16;++frame) {
            if(composite)renderer.beginCompositeFrame(clear,160,120);else renderer.beginFrame(clear);
            renderer.render(scene);
            CHECK(renderer.getFrameStats().graph.compileSucceeded);
            if(frame>=9)CHECK(renderer.captureScreenshot(path.string()));
            renderer.endFrame();
        }
        auto image=decodeImageFile(path.string());CHECK(image.width==160 && image.height==120);
        if(image.rgba8.size()!=160*120*4) {CHECK(false);continue;}
        const float rgb[4]={clear.r,clear.g,clear.b,clear.a};int badBackground=0,badOutside=0;
        for(int y=0;y<120;++y)for(int x=0;x<160;++x) {
            bool inside=x>=ox&&y>=oy&&x<ox+w&&y<oy+h;
            int lx=x-ox,ly=y-oy;
            // Exclude the centered surface; this samples every Bayer phase in
            // empty pixels, plus the untouched chrome around an offset viewport.
            if(inside && lx>w/4 && lx<3*w/4 && ly>h/4 && ly<3*h/4)continue;
            for(int channel=0;channel<4;++channel) {
                float expected=float(uint8_t(rgb[channel]*255))/255;
                if(inside&&style&&channel<3) {
                    float offset=(bayer[(ly%4)*4+lx%4]+.5f)/16-.5f;
                    expected=std::floor(std::clamp(expected*3+offset,0.0f,3.0f)+.5f)/3;
                }
                if(std::abs(int(image.rgba8[(y*160+x)*4+channel])-int(std::lround(expected*255)))>1) {
                    if(inside)++badBackground;else ++badOutside;
                }
            }
        }
        std::cout<<"[ViewportColorParity] "<<path.string()<<" backgroundErrors="<<badBackground<<" chromeErrors="<<badOutside<<'\n';
        CHECK(badBackground==0);CHECK(badOutside==0);
        if(!deferred)forwardImages[mode]=image;
        else if(!style) {
            int maxDifference=0;
            for(int y=oy+h/2-5;y<oy+h/2+5;++y)for(int x=ox+w/2-5;x<ox+w/2+5;++x)for(int c=0;c<3;++c)
                maxDifference=std::max(maxDifference,std::abs(int(image.rgba8[(y*160+x)*4+c])-int(forwardImages[mode].rgba8[(y*160+x)*4+c])));
            std::cout<<"[PbrColorParity] mode="<<mode<<" maxCenterByteError="<<maxDifference<<'\n';
            CHECK(maxDifference<=2);
        }
    }
    renderer.destroyMaterial(material);renderer.destroyMesh(mesh);renderer.shutdown();
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(color_stylization_real_pass_matches_independent_pixel_and_bayer_oracle) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0]) SKIP_TEST("Isolated D3D11/12 device required");
    PhoskiaGpuFixture f; const bool initialized=f.initialize(backend);
    CHECK(initialized); if(!initialized) return;
    configureRenderViewOrder(f.adapter);
    std::array<std::array<uint8_t,4>,16> data;
    for(int y=0;y<4;++y) for(int x=0;x<4;++x)
        data[y*4+x]={uint8_t(37+x*41+y*9),uint8_t(54+y*39+x*7),uint8_t(173-x*17-y*5),uint8_t(53+x*7+y*11)};
    auto source=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(data.data(),sizeof(data)));
    f.textures.push_back(source);
    auto sourceFbo=bgfx::createFrameBuffer(1,&source,false);
    auto readback=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST);
    f.textures.push_back(readback);
    CHECK(bgfx::isValid(sourceFbo)); CHECK(bgfx::isValid(readback));
    struct FboCleanup {bgfx::FrameBufferHandle h;~FboCleanup(){if(bgfx::isValid(h))bgfx::destroy(h);}} cleanup{sourceFbo};
    FrameGraph graph(f.adapter); ColorStylizationPass pass;
    struct PassCleanup {ColorStylizationPass& p;BGFXAdapter& a;~PassCleanup(){p.destroyResources(a);}} cleanupPass{pass,f.adapter};
    FrameContext frame;ayt::render::RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;std::unordered_map<uint64_t,GpuMaterial> materials;
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame,0};ctx.frameGraph=&graph;
    const int bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for(int variant=0;variant<13;++variant) {
        auto variantSource=sourceFbo;
        FboCleanup variantCleanup{BGFX_INVALID_HANDLE};
        ayt::render::ColorStylizationSettings settings;
        settings.enabled=true;settings.pixelationEnabled=variant>=2;
        settings.pixelSize=variant==2 ? 3 : variant==3 ? 2 : 64;
        settings.posterizationEnabled=variant!=2 && variant!=4;
        settings.colorLevels=variant==3 ? 3 : 4;
        settings.ditherStrength=variant==0 ? 0 : variant==3 ? .35f : 1;
        if(variant==5) {
            settings.pixelationEnabled=false;settings.posterizationEnabled=false;
            settings.colorMapping=ayt::render::StylizationColorMapping::FixedPalette;
            settings.ditherStrength=0;settings.paletteSize=4;
            settings.palette[0]={.05f,.1f,.15f};settings.palette[1]={.2f,.4f,.6f};
            settings.palette[2]={.8f,.2f,.4f};settings.palette[3]={.9f,.8f,.3f};
        }
        if(variant==6) {
            settings.pixelationEnabled=false;settings.posterizationEnabled=false;
            settings.colorMapping=ayt::render::StylizationColorMapping::Duotone;
            settings.ditherStrength=0;settings.shadowColor={.1f,.2f,.3f};settings.highlightColor={.8f,.9f,1};
        }
        if(variant>=7) {
            settings.pixelationEnabled=false;settings.posterizationEnabled=false;
            settings.printPattern=static_cast<ayt::render::StylizationPrintPattern>(1+(variant-7)/2);
            const uint8_t tone=variant%2?0:255;
            for(auto& pixel:data)pixel={tone,tone,tone,93};
            // Immutable initialized inputs: updating an RT texture in place is
            // not a portable way to upload the next reference image.
            auto input=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA8,
                BGFX_TEXTURE_RT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(data.data(),sizeof(data)));
            f.textures.push_back(input);variantSource=bgfx::createFrameBuffer(1,&input,false);
            variantCleanup.h=variantSource;CHECK(bgfx::isValid(variantSource));
        }
        pass.setSettings(settings);
        graph.beginFrame(4,4);graph.importExternal(FgResourceId::FinalLdrColor,variantSource);
        graph.addResource(FgResourceId::StylizedColor,{bgfx::TextureFormat::RGBA8,FgTextureScale::Full,true,false,true});
        graph.addPass({"ColorStylization",{FgResourceId::FinalLdrColor},{FgResourceId::StylizedColor},true});
        graph.addPass({"Present",{FgResourceId::StylizedColor},{},true,true});
        graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
        CHECK(graph.compile());
        CHECK(pass.execute(ctx)==0); // A retained valid handle is not current production.
        CHECK_FALSE(graph.producedThisFrame(FgResourceId::StylizedColor));
        graph.markProduced(FgResourceId::FinalLdrColor);
        CHECK(pass.execute(ctx)==1);
        CHECK(graph.producedThisFrame(FgResourceId::StylizedColor));
        auto result=graph.resolveSemantic(FgSemantic::PresentSource);
        CHECK(result.idx==graph.resolve(FgResourceId::StylizedColor).idx);
        bgfx::blit(55,readback,0,0,f.adapter.getFboAttachment(result,0));
        std::array<std::array<uint8_t,4>,16> actual{};
        const auto ready=bgfx::readTexture(readback,actual.data());
        for(int i=0;i<120 && f.adapter.gpuFrameCounter()<ready;++i)f.adapter.endFrame();
        CHECK(f.adapter.gpuFrameCounter()>=ready);
        int mismatch=0;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x) {
            int block=settings.pixelationEnabled ? settings.pixelSize : 1;
            int bx=x/block,by=y/block;
            int sx=bx*block+(std::min(block,4-bx*block)-1)/2;
            int sy=by*block+(std::min(block,4-by*block)-1)/2;
            const auto original=data[sy*4+sx];
            float reference[3]={original[0]/255.f,original[1]/255.f,original[2]/255.f};
            if(variant==5) {
                const float weights[3]={.2126f,.7152f,.0722f};float best=1000;int nearest=0;
                for(int index=0;index<settings.paletteSize;++index) {
                    const auto entry=settings.palette[index];const float rgb[3]={entry.r,entry.g,entry.b};
                    float distance=0;for(int c=0;c<3;++c)distance+=weights[c]*(rgb[c]-reference[c])*(rgb[c]-reference[c]);
                    if(distance<best){best=distance;nearest=index;}
                }
                const auto chosen=settings.palette[nearest];reference[0]=chosen.r;reference[1]=chosen.g;reference[2]=chosen.b;
            }
            if(variant==6) {
                float luminance=reference[0]*.2126f+reference[1]*.7152f+reference[2]*.0722f;
                reference[0]=.1f+.7f*luminance;reference[1]=.2f+.7f*luminance;reference[2]=.3f+.7f*luminance;
            }
            if(variant>=7) {
                const auto color=variant%2?settings.inkColor:settings.paperColor;
                reference[0]=color.r;reference[1]=color.g;reference[2]=color.b;
            }
            for(int c=0;c<4;++c) {
                float expected=c<3?reference[c]:original[3]/255.f;
                if(c<3 && settings.posterizationEnabled) {
                    float offset=((bayer[(by%4)*4+bx%4]+.5f)/16-.5f)*settings.ditherStrength;
                    expected=std::floor(std::clamp(expected*(settings.colorLevels-1)+offset,0.0f,float(settings.colorLevels-1))+.5f)/(settings.colorLevels-1);
                }
                if(std::abs(int(actual[y*4+x][c])-int(std::lround(expected*255)))>1) ++mismatch;
            }
        }
        if(mismatch)std::cerr<<"[Stylization] variant="<<variant<<" mismatches="<<mismatch<<" first="<<int(actual[0][0])<<','<<int(actual[0][1])<<','<<int(actual[0][2])<<','<<int(actual[0][3])<<'\n';
        CHECK(mismatch==0);
    }
    auto disabled=ayt::render::colorStylizationPreset(ayt::render::ColorStylizationPreset::Neutral);
    pass.setSettings(disabled);CHECK(pass.execute(ctx)==0);
    pass.destroyResources(f.adapter);pass.destroyResources(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(stylized_outline_real_pass_depth_normal_and_luminance_edges) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0])SKIP_TEST("Isolated D3D11/12 device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    configureRenderViewOrder(f.adapter);
    CHECK_FALSE(bgfx::isValid(f.adapter.getFboAttachment(BGFX_INVALID_HANDLE,1)));
    FrameGraph graph(f.adapter);RenderResourceBlackboard bb;StylizedOutlinePass pass;
    struct Cleanup{StylizedOutlinePass& p;BGFXAdapter& a;~Cleanup(){p.destroyResources(a);}} cleanup{pass,f.adapter};
    FrameContext frame;ayt::render::RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame,0};
    ctx.frameGraph=&graph;ctx.resourceBlackboard=&bb;
    auto readback=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST);
    f.textures.push_back(readback);CHECK(bgfx::isValid(readback));
    for(int mode=1;mode<=3;++mode) {
        std::array<Pixel,16> colors,normals,depths;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x) {
            const float tone=mode==1?(x<2?.25f:.75f):1.f;
            colors[y*4+x]={tone,tone,tone,.4f};
            depths[y*4+x]={mode==2?(x<2?.2f:.8f):.3f,0,0,1};
            normals[y*4+x]=mode==3 && x>=2?Pixel{.5f,1,.5f,1}:Pixel{.5f,.5f,1,1};
        }
        const auto texture=[&](const auto& data){
            auto h=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,
                BGFX_TEXTURE_RT|BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,
                bgfx::copy(data.data(),sizeof(data)));f.textures.push_back(h);return h;
        };
        const auto color=texture(colors),normal=texture(normals),depth=texture(depths);
        auto source=bgfx::createFrameBuffer(1,&color,false);CHECK(bgfx::isValid(source));
        struct FboCleanup{bgfx::FrameBufferHandle h;~FboCleanup(){if(bgfx::isValid(h))bgfx::destroy(h);}} sourceCleanup{source};
        bb.beginFrame();bb.publishGBuffer(source,color,normal,color,color,depth,4,4,1,true);
        ayt::render::ColorStylizationSettings s;s.enabled=true;s.pixelationEnabled=false;s.posterizationEnabled=false;
        s.outlineMode=static_cast<ayt::render::StylizationOutlineMode>(mode);s.outlineColor={0,0,0};
        pass.setSettings(s);
        graph.beginFrame(4,4);graph.importExternal(FgResourceId::FinalLdrColor,source);
        graph.addResource(FgResourceId::OutlinedColor,{bgfx::TextureFormat::RGBA8,FgTextureScale::Full,true,false,true});
        graph.addPass({"Outline",{FgResourceId::FinalLdrColor},{FgResourceId::OutlinedColor},true});
        graph.addPass({"Present",{FgResourceId::OutlinedColor},{},true,true});
        graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
        CHECK(graph.compile());CHECK(pass.execute(ctx)==0);graph.markProduced(FgResourceId::FinalLdrColor);
        if(mode>1){bb.invalidateFrameOutputs(ResourceInvalidationReason::ProducerFailure);CHECK(pass.execute(ctx)==0);
            bb.publishGBuffer(source,color,normal,color,color,depth,4,4,2,true);}
        CHECK(pass.execute(ctx)==1);
        const auto output=graph.resolveSemantic(FgSemantic::PresentSource);
        CHECK(output.idx==graph.resolve(FgResourceId::OutlinedColor).idx);
        bgfx::blit(55,readback,0,0,f.adapter.getFboAttachment(output,0));
        std::array<std::array<uint8_t,4>,16> actual{};const auto ready=bgfx::readTexture(readback,actual.data());
        for(int i=0;i<120 && f.adapter.gpuFrameCounter()<ready;++i)f.adapter.endFrame();
        CHECK(f.adapter.gpuFrameCounter()>=ready);
        int differences=0;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x)for(int c=0;c<4;++c) {
            const int expected=c==3?102:(x==1 || x==2)?0:int(std::lround(colors[y*4+x][c]*255));
            if(std::abs(int(actual[y*4+x][c])-expected)>1)++differences;
        }
        if(differences)std::cerr<<"Outline mode="<<mode<<" mismatches="<<differences<<'\n';
        CHECK(differences==0);
        for(int i=0;i<3;++i)f.adapter.endFrame();
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(analog_display_real_pass_physical_mask_and_channel_offsets) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    configureRenderViewOrder(f.adapter);
    std::array<std::array<uint8_t,4>,16> pixels;
    for(int y=0;y<4;++y)for(int x=0;x<4;++x)pixels[y*4+x]={uint8_t(40+40*x),uint8_t(80+20*y),uint8_t(160-30*x),uint8_t(60+10*x+15*y)};
    auto input=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_RT|BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(pixels.data(),sizeof(pixels)));
    auto source=bgfx::createFrameBuffer(1,&input,false);f.textures.push_back(input);CHECK(bgfx::isValid(source));
    struct SourceCleanup{bgfx::FrameBufferHandle h;~SourceCleanup(){if(bgfx::isValid(h))bgfx::destroy(h);}} sourceCleanup{source};
    auto readback=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA8,BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST);
    f.textures.push_back(readback);CHECK(bgfx::isValid(readback));
    AnalogDisplayPass pass;FrameGraph graph(f.adapter);
    struct PassCleanup{AnalogDisplayPass& p;BGFXAdapter& a;~PassCleanup(){p.destroyResources(a);}} passCleanup{pass,f.adapter};
    FrameContext frame;ayt::render::RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame,0};ctx.frameGraph=&graph;
    for(int variant=0;variant<4;++variant) {
        ayt::render::ColorStylizationSettings look;look.enabled=true;auto& s=look.display;s.enabled=true;
        s.scanlineStrength=s.maskStrength=s.chromaticOffset=s.curvature=s.vignetteStrength=s.grainStrength=0;
        if(variant==0)s.scanlineStrength=.5f;
        if(variant==1)s.maskStrength=.5f;
        if(variant==2)s.chromaticOffset=1;
        if(variant==3){s.grainStrength=.2f;s.noiseSeed=35717;}
        pass.setSettings(look);
        graph.beginFrame(4,4);graph.importExternal(FgResourceId::FinalLdrColor,source);
        graph.addResource(FgResourceId::DisplayColor,{bgfx::TextureFormat::RGBA8,FgTextureScale::Full,true,false,true});
        graph.addPass({"AnalogDisplay",{FgResourceId::FinalLdrColor},{FgResourceId::DisplayColor},true});
        graph.addPass({"Present",{FgResourceId::DisplayColor},{},true,true});
        graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
        CHECK(graph.compile());CHECK(pass.execute(ctx)==0);graph.markProduced(FgResourceId::FinalLdrColor);
        CHECK(pass.execute(ctx)==1);const auto result=graph.resolveSemantic(FgSemantic::PresentSource);
        CHECK(result.idx==graph.resolve(FgResourceId::DisplayColor).idx);
        bgfx::blit(55,readback,0,0,f.adapter.getFboAttachment(result,0));
        std::array<std::array<uint8_t,4>,16> actual{};const auto ready=bgfx::readTexture(readback,actual.data());
        for(int i=0;i<120 && f.adapter.gpuFrameCounter()<ready;++i)f.adapter.endFrame();
        CHECK(f.adapter.gpuFrameCounter()>=ready);int mismatch=0;
        for(int y=0;y<4;++y)for(int x=0;x<4;++x)for(int c=0;c<4;++c) {
            float expected=pixels[y*4+x][c];
            if(c<3 && variant==0 && y%2)expected*=.5f;
            if(c<3 && variant==1 && c!=x%3)expected*=.5f;
            if(variant==2 && c==0)expected=pixels[y*4+std::min(x+1,3)][0];
            if(variant==2 && c==2)expected=pixels[y*4+std::max(x-1,0)][2];
            if(variant==3 && c<3) {
                const int u=(x+int(s.noiseSeed)%251)%251,v=(y+int(s.noiseSeed)/251)%251;
                int rank=(u*67+v*131)%251;rank=(rank*rank*3+u*v*11+u*29+v*73)%251;
                const float noise=(rank+.5f)/251-.5f;
                expected=std::clamp(expected+noise*.2f*255,0.f,255.f);
            }
            if(std::abs(int(actual[y*4+x][c])-int(std::lround(expected)))>1)++mismatch;
        }
        if(mismatch)std::cerr<<"Display variant="<<variant<<" mismatches="<<mismatch<<'\n';
        CHECK(mismatch==0);
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(deferred_decal_real_pass_ping_pong_resize_and_borrowed_depth_lifetime) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    auto grid=[&](Pixel value){std::array<Pixel,16> data;data.fill(value);auto h=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_RT|BGFX_SAMPLER_POINT,bgfx::copy(data.data(),sizeof(data)));f.textures.push_back(h);return h;};
    auto a=grid({.2f,.4f,.6f,.3f}),n=grid({.5f,.5f,1,.8f}),w=grid({0,0,0,.4f}),surface=grid({.1f,.2f,.3f,1});
    auto depth=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::D24S8,BGFX_TEXTURE_RT);
    CHECK(bgfx::isValid(depth));f.textures.push_back(depth);
    bgfx::TextureHandle attachments[5]={a,n,w,surface,depth};
    auto base=bgfx::createFrameBuffer(5,attachments,false);CHECK(bgfx::isValid(base));
    struct BaseCleanup{bgfx::FrameBufferHandle h;~BaseCleanup(){if(bgfx::isValid(h))bgfx::destroy(h);}} cleanup{base};
    f.adapter.setViewFrameBuffer(50,base);f.adapter.setViewRect(50,0,0,4,4);
    f.adapter.setViewClearRaw(50,BGFX_CLEAR_DEPTH,0,.37f,0);bgfx::touch(50);f.adapter.endFrame();
    f.adapter.setViewClearRaw(50,BGFX_CLEAR_NONE,0,1,0);
    auto checkDepth=[&](){checkPixels(f.render("texture2d sampledDepth","return vec4(vec3(sample(sampledDepth,vec2(0.5)).r),1.0)",
        [&](const auto& shader){auto b=shader.getTextureBinding("sampledDepth");shader.setTexture(shader.getTextureStage(b),b,toShaderTexture(depth));}),{.37f,.37f,.37f,1});};
    FrameContext frame;frame.decalsEnabled=true;ayt::render::RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;std::unordered_map<uint64_t,GpuMaterial> materials;
    RenderResourceBlackboard bb;FrameGraph graph(f.adapter);DeferredDecalPass pass;
    struct PassCleanup{DeferredDecalPass& p;BGFXAdapter& a;~PassCleanup(){p.destroyResources(a);}} passCleanup{pass,f.adapter};
    ayt::render::DeferredDecal red,blue;red.color={1,0,0};blue.color={0,0,1};red.opacity=blue.opacity=.5f;
    std::vector<ayt::render::DeferredDecal> decals={red,blue};
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame,0,base};
    ctx.resourceBlackboard=&bb;ctx.frameGraph=&graph;ctx.decals=&decals;
    for(int i=0;i<3;++i)f.adapter.endFrame();auto baseline=bgfx::getStats()->numTextures;
    for(int cycle=0;cycle<3;++cycle) {
        CHECK(pass.prepare(f.adapter,4,4,depth,cycle+1));CHECK(bgfx::isValid(pass.output(2)));
        bb.beginFrame();bb.publishGBuffer(base,a,n,w,surface,depth,4,4,cycle+1,true);
        PostProcessGraphPlanInput in;in.width=in.height=4;in.sceneColor=base;in.gbuffer=pass.output(2);in.baseGBuffer=base;in.decals=true;
        CHECK(buildPostProcessGraphPlan(graph,in).compileSucceeded);CHECK(pass.execute(ctx)==2);
        BlackboardGBufferView result;CHECK(bb.resolveGBuffer(result));CHECK(result.framebuffer.idx==pass.output(2).idx);CHECK(result.depth.idx==depth.idx);
        for(int i=0;i<3;++i)f.adapter.endFrame();CHECK(bgfx::getStats()->numTextures==baseline+8);
        checkDepth(); // Neither a fullscreen clear nor WRITE_Z may alter base depth.
        auto pixels=f.render("texture2d result", "return sample(result,vec2(0.5))",[&](const auto& shader){auto b=shader.getTextureBinding("result");shader.setTexture(shader.getTextureStage(b),b,toShaderTexture(result.albedo));});
        CHECK(pixels.size()==16);for(const auto& p:pixels){CHECK(std::abs(p[0]-.3f)<.008f);CHECK(std::abs(p[1]-.1f)<.008f);CHECK(std::abs(p[2]-.65f)<.008f);}
        // Original group remains authoritative if any later producer cannot bind.
        bb.beginFrame();bb.publishGBuffer(base,a,n,w,surface,depth,4,4,cycle+1,true);
        decals[0].albedo.id=999999;CHECK(pass.execute(ctx)==0);CHECK(bb.resolveGBuffer(result));CHECK(result.framebuffer.idx==base.idx);decals[0].albedo={};
        frame.decalsEnabled=false;CHECK(pass.execute(ctx)==0);frame.decalsEnabled=true;
    }
    // Genuine size changes recreate private colors while retaining borrowed depth.
    auto smallDepth=bgfx::createTexture2D(2,2,false,1,bgfx::TextureFormat::D24S8,BGFX_TEXTURE_RT);
    CHECK(bgfx::isValid(smallDepth));
    CHECK(pass.prepare(f.adapter,2,2,smallDepth,4));CHECK(pass.prepare(f.adapter,4,4,depth,5));
    bgfx::destroy(smallDepth);
    pass.destroyResources(f.adapter);for(int i=0;i<3;++i)f.adapter.endFrame();CHECK(bgfx::getStats()->numTextures==baseline);
    checkDepth();
    // Borrowed base depth/FBO remain valid and can be sampled after decal teardown.
    auto pixels=f.render("texture2d original","return sample(original,vec2(0.5))",[&](const auto& shader){auto b=shader.getTextureBinding("original");shader.setTexture(shader.getTextureStage(b),b,toShaderTexture(a));});
    checkPixels(pixels,{.2f,.4f,.6f,.3f});
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(deferred_decal_culling_preserves_order_parity_and_complete_transaction_validation) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Isolated D3D11/12 device required");
    PhoskiaGpuFixture f;
    const bool initialized = f.initialize(backend);
    CHECK(initialized); if (!initialized) return;
    auto grid = [&](Pixel value) {
        std::array<Pixel, 16> data; data.fill(value);
        auto h = bgfx::createTexture2D(4, 4, false, 1, bgfx::TextureFormat::RGBA32F,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_POINT, bgfx::copy(data.data(), sizeof(data)));
        CHECK(bgfx::isValid(h)); f.textures.push_back(h); return h;
    };
    auto a = grid({.2f,.4f,.6f,.3f}), n = grid({.5f,.5f,1,.8f});
    auto w = grid({0,0,.5f,.4f}), surface = grid({.1f,.2f,.3f,1});
    auto depth = bgfx::createTexture2D(4, 4, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
    CHECK(bgfx::isValid(depth)); f.textures.push_back(depth);
    bgfx::TextureHandle attachments[5] = {a,n,w,surface,depth};
    auto base = bgfx::createFrameBuffer(5, attachments, false);
    CHECK(bgfx::isValid(base));
    struct BaseCleanup { bgfx::FrameBufferHandle h; ~BaseCleanup(){bgfx::destroy(h);} } baseCleanup{base};
    FrameContext frame; frame.decalsEnabled = true; frame.frustumCulling = true;
    ayt::render::RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;
    std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    RenderResourceBlackboard bb; FrameGraph graph(f.adapter); DeferredDecalPass pass;
    struct PassCleanup { DeferredDecalPass& p; BGFXAdapter& a; ~PassCleanup(){p.destroyResources(a);} } passCleanup{pass,f.adapter};
    ayt::render::DeferredDecal red, blue, offscreen;
    red.color = {1,0,0}; blue.color = {0,0,1}; red.opacity = blue.opacity = .5f;
    offscreen.localToWorld(0,3) = 10;
    std::vector<ayt::render::DeferredDecal> decals{offscreen, red, offscreen, blue, offscreen};
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame,0,base};
    ctx.resourceBlackboard = &bb; ctx.frameGraph = &graph; ctx.decals = &decals;
    auto dispatch = [&](size_t expected) {
        const auto selected = selectDeferredDecals(decals, frame);
        CHECK(selected.count == expected);
        if (expected) CHECK(pass.prepare(f.adapter,4,4,depth,1));
        bb.beginFrame(); bb.publishGBuffer(base,a,n,w,surface,depth,4,4,1,true);
        PostProcessGraphPlanInput input;
        input.width = input.height = 4; input.sceneColor = base;
        input.decals = expected != 0; input.baseGBuffer = base;
        input.gbuffer = expected ? pass.output(expected) : base;
        CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
        CHECK(graph.shouldExecute(ayt::render::RenderPassSlot::DeferredDecal) == (expected != 0));
        CHECK(pass.execute(ctx) == expected);
        BlackboardGBufferView result; CHECK(bb.resolveGBuffer(result));
        CHECK(result.framebuffer.idx == (expected ? pass.output(expected).idx : base.idx));
        CHECK(result.depth.idx == depth.idx);
        CHECK(graph.producedThisFrame(FgResourceId::GBufferAlbedo) == (expected != 0));
        for (int i = 0; i < 3; ++i) f.adapter.endFrame();
        return f.render("texture2d result", "return sample(result,vec2(0.5))",
            [&](const auto& shader) { auto b=shader.getTextureBinding("result");
                shader.setTexture(shader.getTextureStage(b),b,toShaderTexture(result.albedo)); });
    };
    auto optimized = dispatch(2); // Source count 5, final target changes parity.
    frame.frustumCulling = false;
    auto baseline = dispatch(5);
    CHECK(optimized.size() == baseline.size());
    for (size_t i = 0; i < optimized.size() && i < baseline.size(); ++i)
        for (size_t c = 0; c < 4; ++c) CHECK(std::abs(optimized[i][c]-baseline[i][c]) < .008f);
    for (const auto& p : optimized) {
        CHECK(std::abs(p[0]-.3f)<.008f); CHECK(std::abs(p[1]-.1f)<.008f); CHECK(std::abs(p[2]-.65f)<.008f);
    }
    frame.frustumCulling = true;
    decals = {offscreen, red}; dispatch(1); // Even source count, odd output count.
    decals = {red, blue, red}; dispatch(3);
    decals = {offscreen, offscreen};
    auto empty = dispatch(0); checkPixels(empty,{.2f,.4f,.6f,.3f});
    // An offscreen descriptor with a missing resource still rejects the whole
    // mixed transaction. Visibility must not hide resource failures.
    decals = {red, offscreen}; decals[1].albedo.id = 999999;
    bb.beginFrame(); bb.publishGBuffer(base,a,n,w,surface,depth,4,4,1,true);
    CHECK(pass.execute(ctx) == 0);
    BlackboardGBufferView result; CHECK(bb.resolveGBuffer(result)); CHECK(result.framebuffer.idx == base.idx);
    decals[1].albedo = {}; decals[1].localToWorld(0,0) = 0;
    CHECK(pass.execute(ctx) == 0); CHECK(bb.resolveGBuffer(result)); CHECK(result.framebuffer.idx == base.idx);
    decals = {red}; frame.decalsEnabled = false;
    CHECK(pass.execute(ctx) == 0); frame.decalsEnabled = true; dispatch(1);
    // Quantized world positions can receive a projector just outside the exact
    // near plane. Exercise the real shader with the binary16-rounded receiver.
    std::array<Pixel,16> nearWorld;
    nearWorld.fill({0,0,.0999755859375f,.4f});
    bgfx::updateTexture2D(w,0,0,0,0,4,4,bgfx::copy(nearWorld.data(),sizeof(nearWorld)));
    frame.cullingViewProjection = ayt::math::lh::perspective(
        ayt::math::radians(90.0f),1,.1f,10);
    auto tiny = red;
    for(int axis=0;axis<3;++axis) tiny.localToWorld(axis,axis)=.001f;
    tiny.localToWorld(2,3)=.0999755859375f-.00099f;
    decals={tiny}; frame.frustumCulling=false;
    baseline=dispatch(1); frame.frustumCulling=true; optimized=dispatch(1);
    CHECK(optimized.size()==baseline.size());
    for(size_t i=0;i<optimized.size() && i<baseline.size();++i) {
        CHECK(optimized[i][0]>.5f); CHECK(optimized[i][2]<.35f);
        for(size_t c=0;c<4;++c) CHECK(std::abs(optimized[i][c]-baseline[i][c])<.008f);
    }
    pass.destroyResources(f.adapter); decals = {offscreen};
    dispatch(0); CHECK(!bgfx::isValid(pass.output(1))); // Cold all-offscreen path allocates nothing.
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(deferred_decal_culling_fixed_scene_image_and_timing_baseline) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Isolated D3D11/12 device required");
    PhoskiaGpuFixture f;
    const bool initialized = f.initialize(backend);
    CHECK(initialized); if (!initialized) return;
    bgfx::setDebug(BGFX_DEBUG_PROFILER); // View timestamps are opt-in on bgfx.
    constexpr uint16_t width = 1024, height = 576;
    constexpr size_t pixels = size_t(width) * height;
    std::array<bgfx::TextureHandle, 4> colors, staging;
    for (size_t channel = 0; channel < 4; ++channel) {
        const bool world = channel == 2;
        const auto format = world ? bgfx::TextureFormat::RGBA16F : bgfx::TextureFormat::RGBA8;
        std::vector<uint8_t> data(pixels * (world ? 8 : 4));
        for (size_t i = 0; i < pixels; ++i) {
            if (world) {
                const std::array<uint16_t,4> value{0,0,0x3800,0x3400}; // (0,0,.5,.25), exact binary16.
                std::memcpy(data.data()+i*8,value.data(),8);
            } else {
                const std::array<uint8_t,4> value = channel == 0
                    ? std::array<uint8_t,4>{uint8_t(i%251),uint8_t((i/width)%241),uint8_t((i*7)%253),77}
                    : channel == 1 ? std::array<uint8_t,4>{128,128,255,204}
                    : std::array<uint8_t,4>{26,51,77,255};
                std::memcpy(data.data()+i*4,value.data(),4);
            }
        }
        colors[channel] = bgfx::createTexture2D(width,height,false,1,format,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            bgfx::copy(data.data(),uint32_t(data.size())));
        staging[channel] = bgfx::createTexture2D(width,height,false,1,format,
            BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
        CHECK(bgfx::isValid(colors[channel])); CHECK(bgfx::isValid(staging[channel]));
        f.textures.push_back(colors[channel]); f.textures.push_back(staging[channel]);
    }
    auto depth = bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D24S8,BGFX_TEXTURE_RT);
    CHECK(bgfx::isValid(depth)); f.textures.push_back(depth);
    bgfx::TextureHandle attachments[5]{colors[0],colors[1],colors[2],colors[3],depth};
    auto base = bgfx::createFrameBuffer(5,attachments,false); CHECK(bgfx::isValid(base));
    struct BaseCleanup { bgfx::FrameBufferHandle h; ~BaseCleanup(){bgfx::destroy(h);} } baseCleanup{base};
    FrameContext frame; frame.decalsEnabled = true;
    frame.cullingMarginX = 4.0f/width; frame.cullingMarginY = 4.0f/height;
    ayt::render::RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;
    std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    RenderResourceBlackboard bb; FrameGraph graph(f.adapter); DeferredDecalPass pass;
    struct PassCleanup { DeferredDecalPass& p; BGFXAdapter& a; ~PassCleanup(){p.destroyResources(a);} } passCleanup{pass,f.adapter};
    std::vector<ayt::render::DeferredDecal> decals;
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,width,height,frame,0,base};
    ctx.resourceBlackboard = &bb; ctx.frameGraph = &graph; ctx.decals = &decals;
    auto percentile = [](std::vector<double> values, double fraction) {
        if (values.empty()) return 0.0;
        std::sort(values.begin(),values.end());
        return values[size_t(std::ceil(fraction*values.size()))-1];
    };
    using Image = std::array<std::vector<uint8_t>,4>;
    auto readImages = [&]() {
        BlackboardGBufferView result; CHECK(bb.resolveGBuffer(result));
        const std::array<bgfx::TextureHandle,4> outputs{
            result.albedo,result.normal,result.worldPosition,result.material};
        Image image;
        uint32_t ready = 0;
        for (size_t i = 0; i < outputs.size(); ++i) {
            image[i].resize(pixels*(i==2?8:4));
            bgfx::blit(uint16_t(120+i),staging[i],0,0,outputs[i]);
            ready = std::max(ready,bgfx::readTexture(staging[i],image[i].data()));
        }
        for (int i = 0; i < 120 && f.adapter.gpuFrameCounter() < ready; ++i) f.adapter.endFrame();
        CHECK(f.adapter.gpuFrameCounter() >= ready);
        return image;
    };
    const char* scenarios[] = {"none","all-visible","mixed","all-offscreen"};
    for (int scenario = 0; scenario < 4; ++scenario) {
        decals.assign(scenario == 0 ? 0 : 16,{});
        for (size_t i = 0; i < decals.size(); ++i) {
            decals[i].color = {float(i%3)/2,float((i+1)%3)/2,float((i+2)%3)/2};
            decals[i].opacity = .15f;
            const bool visible = scenario == 1 || (scenario == 2 && i%3 == 1);
            if (!visible) decals[i].localToWorld(0,3) = 10+float(i);
        }
        Image baseline;
        for (bool culling : {false,true}) {
            frame.frustumCulling = culling;
            const uint32_t expected = !culling ? uint32_t(decals.size())
                : scenario == 1 ? 16 : scenario == 2 ? 5 : 0;
            std::vector<double> cpuMs, gpuMs, backendDraws;
            std::unordered_set<uint32_t> timedFrames;
            constexpr int warmupFrames = 64, measuredFrames = 32;
            uint32_t measuredFirstFrame = 0;
            // Warm the programs, targets and delayed GPU query pipeline first.
            for (int iteration = 0; iteration < warmupFrames+measuredFrames; ++iteration) {
                if (iteration == warmupFrames) measuredFirstFrame = f.adapter.gpuFrameCounter();
                const auto start = std::chrono::steady_clock::now();
                auto selected = selectDeferredDecals(decals,frame);
                ctx.decalVisibility = &selected;
                CHECK(selected.count == expected);
                if (selected.count) CHECK(pass.prepare(f.adapter,width,height,depth,1));
                bb.beginFrame(); bb.publishGBuffer(base,colors[0],colors[1],colors[2],colors[3],depth,width,height,1,true);
                PostProcessGraphPlanInput input;
                input.width=width; input.height=height; input.sceneColor=base;
                input.decals = selected.count != 0; input.baseGBuffer = base;
                input.gbuffer = selected.count ? pass.output(selected.count) : base;
                CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
                const auto submitted = graph.shouldExecute(ayt::render::RenderPassSlot::DeferredDecal) ? pass.execute(ctx) : 0;
                CHECK(submitted == expected);
                const double elapsed = std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-start).count();
                f.adapter.endFrame();
                if (iteration < warmupFrames) continue;
                cpuMs.push_back(elapsed);
                const auto* stats = bgfx::getStats();
                backendDraws.push_back(stats->numDraw);
                double gpu = 0;
                uint32_t timingFrame = UINT32_MAX, timedViews = 0;
                bool coherent = true;
                for (uint16_t view = 0; view < stats->numViews; ++view) {
                    const auto& value = stats->viewStats[view];
                    if (value.view >= DeferredDecalPass::kFirstView
                        && value.view < DeferredDecalPass::kFirstView+ayt::render::kMaxDeferredDecals
                        && value.gpuTimeEnd > value.gpuTimeBegin && stats->gpuTimerFreq > 0) {
                        if (timedViews && timingFrame != value.gpuFrameNum) coherent = false;
                        timingFrame = value.gpuFrameNum; ++timedViews;
                        gpu += double(value.gpuTimeEnd-value.gpuTimeBegin)*1000.0/stats->gpuTimerFreq;
                    }
                }
                if (gpu > 0 && coherent && timedViews == expected
                    && timingFrame >= measuredFirstFrame && timingFrame < f.adapter.gpuFrameCounter()
                    && timedFrames.insert(timingFrame).second) gpuMs.push_back(gpu);
            }
            CHECK(percentile(backendDraws,.5) == expected);
            std::cout << "[DecalCullingBaseline] {\"backend\":\"" << backend
                << "\",\"scenario\":\"" << scenarios[scenario] << "\",\"culling\":" << (culling?"true":"false")
                << ",\"width\":" << width << ",\"height\":" << height << ",\"samples\":" << cpuMs.size()
                << ",\"warmupFrames\":" << warmupFrames
                << ",\"vendorId\":" << bgfx::getCaps()->vendorId << ",\"deviceId\":" << bgfx::getCaps()->deviceId
                << ",\"measuredFirstFrame\":" << measuredFirstFrame << ",\"submittedEndFrame\":" << f.adapter.gpuFrameCounter()
                << ",\"draws\":" << expected << ",\"backendDrawsMedian\":" << percentile(backendDraws,.5)
                << ",\"cpuSubmitMedianMs\":" << percentile(cpuMs,.5) << ",\"cpuSubmitP95Ms\":" << percentile(cpuMs,.95)
                << ",\"gpuTimingSamples\":" << gpuMs.size() << ",\"gpuViewsMedianMs\":" << percentile(gpuMs,.5)
                << ",\"gpuViewsP95Ms\":" << percentile(gpuMs,.95) << "}\n";
            const auto image = readImages();
            if (!culling) { baseline = image; continue; }
            for (size_t channel = 0; channel < image.size(); ++channel) {
                CHECK(image[channel].size() == baseline[channel].size());
                size_t differences = 0; unsigned maxError = 0;
                for (size_t i = 0; i < image[channel].size(); ++i) {
                    const unsigned error = unsigned(std::abs(int(image[channel][i])-int(baseline[channel][i])));
                    maxError = std::max(maxError,error);
                    if (error > (channel==2?0u:1u)) ++differences;
                }
                CHECK(differences == 0);
                std::cout << "[DecalCullingImage] {\"backend\":\"" << backend << "\",\"scenario\":\""
                    << scenarios[scenario] << "\",\"attachment\":" << channel
                    << ",\"bytes\":" << image[channel].size() << ",\"maxByteError\":" << maxError
                    << ",\"outsideTolerance\":" << differences << "}\n";
            }
        }
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(deferred_decal_main_renderer_graph_bypass_resize_and_final_pixels) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Isolated D3D11/12 device required");
    HWND window = CreateWindowExW(0,L"STATIC",L"Decal culling integration",WS_OVERLAPPED,
        0,0,256,256,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window != nullptr); if (!window) return;
    struct WindowCleanup { HWND h; ~WindowCleanup(){DestroyWindow(h);} } windowCleanup{window};
    ayt::render::Renderer renderer;
    ayt::render::InitDesc init; init.windowHandle = window; init.width = init.height = 256;
    init.msaa = 0; init.vsync = false;
    init.backend = std::string(backend)=="d3d12" ? ayt::render::Backend::Direct3D12 : ayt::render::Backend::Direct3D11;
    const bool initialized = renderer.initialize(init); CHECK(initialized); if (!initialized) return;
    renderer.configurePipeline(ayt::render::RenderPipelineDesc::makeDeferred());
    renderer.setShadowsEnabled(false); renderer.setFxaaEnabled(false);
    renderer.setSmaaEnabled(false); renderer.setTaaEnabled(false);
    renderer.setMainCamera(ayt::math::Float4x4::identity(),ayt::math::Float4x4::identity());
    auto material = renderer.createMaterialFromPhoskia(R"(material DecalReceiver {
        property baseColor = vec4(0.2,0.4,0.6,1.0)
        vertex { in pos:position return modelViewProjection*vec4(pos,1.0) }
        fragment { return baseColor }
    })","decal_culling_receiver");
    CHECK(material.isValid());
    renderer.setMaterialColor(material,"baseColor",.2f,.4f,.6f,1);
    auto mesh = renderer.createUnitCube(); CHECK(mesh.isValid());
    ayt::render::RenderScene scene;
    ayt::render::DrawItem item; item.mesh = mesh; item.material = material;
    item.world = ayt::math::translate(0,0,.5f)*ayt::math::scale(.6f,.6f,.2f);
    scene.add(item);
    ayt::render::DeferredDecal red,blue,offscreen;
    red.color={1,0,0}; blue.color={0,0,1}; red.opacity=blue.opacity=.5f;
    red.localToWorld(0,0)=red.localToWorld(2,2)=-1;
    red.localToWorld(2,3)=.5f; blue.localToWorld=red.localToWorld;
    offscreen=red; offscreen.localToWorld(0,3)=10;
    const char* directory = std::getenv("AY_DECAL_CAPTURE_DIRECTORY");
    const auto root = directory ? std::filesystem::path(directory)/backend
        : std::filesystem::temp_directory_path()/(std::string("ay-decal-culling-")+backend+"-"
            +std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    auto renderFrames = [&](uint32_t expected) {
        for (int i=0;i<6;++i) {
            renderer.beginFrame(); renderer.render(scene); renderer.endFrame();
            CHECK(renderer.getFrameStats().graph.compileSucceeded);
            uint32_t actual = 0;
            for (const auto& pass : renderer.getFrameStats().passes)
                if (pass.name=="DeferredDecal") actual+=pass.drawCalls;
            CHECK(actual==expected);
        }
    };
    auto capture = [&](const char* label,uint32_t expected) {
        renderFrames(expected);
        const auto path = root/(std::string(label)+".png");
        for (int i=0;i<6;++i) {
            renderer.beginFrame(); renderer.render(scene);
            CHECK(renderer.captureScreenshot(path.string())); renderer.endFrame();
        }
        auto image = decodeImageFile(path.string()); CHECK(!image.rgba8.empty());
        std::cout << "[DecalCullingCapture] " << path.string() << '\n';
        return image;
    };
    auto compare = [&](const auto& a,const auto& b) {
        CHECK(a.width==b.width); CHECK(a.height==b.height); CHECK(a.rgba8.size()==b.rgba8.size());
        size_t differences=0; unsigned maxError=0;
        for (size_t i=0;i<a.rgba8.size() && i<b.rgba8.size();++i) {
            const unsigned error=unsigned(std::abs(int(a.rgba8[i])-int(b.rgba8[i])));
            maxError=std::max(maxError,error); if(error>1)++differences;
        }
        CHECK(differences==0);
        std::cout<<"[DecalCullingFinalImage] backend="<<backend<<" maxByteError="<<maxError
                 <<" outsideTolerance="<<differences<<'\n';
    };
    auto plain=capture("disabled",0);
    const auto coldTextures=bgfx::getStats()->numTextures;
    renderer.setDeferredDecals({offscreen,offscreen}); renderer.setDeferredDecalsEnabled(true);
    auto invisible=capture("all-offscreen",0);
    CHECK(bgfx::getStats()->numTextures==coldTextures); // No private MRT allocation on cold bypass.
    compare(plain,invisible);
    renderer.setDeferredDecals({offscreen,red,offscreen,blue,offscreen});
    renderer.setFrustumCullingEnabled(false);
    auto baseline=capture("mixed-baseline",5);
    renderer.setFrustumCullingEnabled(true);
    auto optimized=capture("mixed-culled",2); compare(baseline,optimized);
    size_t changed=0;
    for(size_t i=0;i<plain.rgba8.size() && i<optimized.rgba8.size();++i)
        if(std::abs(int(plain.rgba8[i])-int(optimized.rgba8[i]))>4)++changed;
    CHECK(changed>100); // The integration must really contain receiving geometry.
    renderer.resize(320,192); renderer.setViewportRect(0,0,320,192);
    renderer.setFrustumCullingEnabled(false); baseline=capture("resized-baseline",5);
    renderer.setFrustumCullingEnabled(true); optimized=capture("resized-culled",2); compare(baseline,optimized);
    renderer.setDeferredDecalsEnabled(false); renderFrames(0);
    renderer.setDeferredDecalsEnabled(true); renderFrames(2);
    renderer.setDeferredDecals({offscreen}); renderFrames(0);
    renderer.setDeferredDecals({red}); renderFrames(1);
    renderer.setTaaEnabled(true); renderFrames(1);
    renderer.setMainCamera(ayt::math::translate(-10,0,0),ayt::math::Float4x4::identity());
    renderer.notifyCameraCut(); renderFrames(0);
    renderer.setMainCamera(ayt::math::Float4x4::identity(),ayt::math::Float4x4::identity());
    renderer.notifyCameraCut(); renderFrames(1);
    renderer.shutdown();
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(deferred_decal_preserves_metadata_and_updates_color_material_normal) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    std::array<bgfx::TextureHandle,4> outputs{f.targetTexture};
    for(int i=1;i<4;++i){outputs[i]=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_RT);CHECK(bgfx::isValid(outputs[i]));f.textures.push_back(outputs[i]);}
    bgfx::destroy(f.target);f.target=bgfx::createFrameBuffer(4,outputs.data(),false);CHECK(bgfx::isValid(f.target));
    auto a=f.input({.2f,.4f,.6f,.3f}),n=f.input({.5f,.5f,1,.8f}),back=f.input({.5f,.5f,0,.8f});
    auto w=f.input({0,0,0,.4f}),surface=f.input({.1f,.2f,.3f,1}),sky=f.input({.1f,.2f,.3f,0});
    auto decal=f.input({.8f,.6f,.4f,.5f}),normal=f.input({.8f,.5f,.9f,1});
    auto p=f.pool.acquire(ayt::render::kDeferredDecalPhoskiaSource,ayt::render::kDeferredDecalCacheKey);
    for(int mode=0;mode<5;++mode) {
        auto pixels=f.renderResource(p,[&](const auto& shader){
            auto tex=[&](const char* name,bgfx::TextureHandle h){auto b=shader.getTextureBinding(name);CHECK(b!=ayt::shader::InvalidBinding);shader.setTexture(shader.getTextureStage(b),b,toShaderTexture(h));};
            tex("sourceAlbedo",a);tex("sourceNormal",mode==4?back:n);tex("sourceWorld",w);tex("sourceSurface",mode==3?sky:surface);tex("decalAlbedo",decal);tex("decalNormal",normal);
            auto matrix=ayt::math::Float4x4::identity();if(mode==0)matrix.row[0].w=3;
            float upload[16];toBgfxColumnMajor(matrix,upload);shader.setUniform(shader.getUniformBinding("decalWorldToLocal"),upload,64);
            auto u=[&](const char* name,Pixel v){auto b=shader.getUniformBinding(name);CHECK(b!=ayt::shader::InvalidBinding);shader.setUniform(b,v.data(),16);};
            u("decalTangent",{1,0,0,0});u("decalBitangent",{0,1,0,0});u("decalFacing",{0,0,1,.1f});
            u("decalTint",{1,mode==2?1.f:0.f,mode==2?1.f:0.f,mode==2?1.f:.5f});
            u("decalMaterial",{.2f,.9f,1,1});u("decalOptions",{mode==2?1.f:0.f,mode==2?1.f:0.f,0,0});
        });
        const bool changed=mode==1||mode==2;
        checkPixels(pixels,!changed?Pixel{.2f,.4f,.6f,.3f}:mode==1?Pixel{.6f,.2f,.3f,.6f}:Pixel{.5f,.5f,.5f,.6f});
        Pixel expectedNormal=mode==4?Pixel{.5f,.5f,0,.8f}:Pixel{.5f,.5f,1,changed?.5f:.8f};
        if(mode==2)expectedNormal={.65811388f,.5f,.97434165f,.5f};
        checkPixels(f.readImage(outputs[1]),expectedNormal);
        checkPixels(f.readImage(outputs[2]),{0,0,0,.4f});
        checkPixels(f.readImage(outputs[3]),{.1f,.2f,.3f,mode==3?0.f:1.f});
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(volumetric_shader_matches_extinction_scattering_local_box_and_shadow) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    auto scene=f.input({2,2,2,1}),world=f.input({0,0,2,1}),sky=f.input({0,0,0,0}),lit=f.input({1,1,1,1}),dark=f.input({0,0,0,1});
    auto program=f.pool.acquire(ayt::render::kVolumetricPhoskiaSource,ayt::render::kVolumetricCacheKey);
    for(int mode=0;mode<6;++mode) {
        const float density=mode==0?0.f:.1f;
        const float distance=mode==2?.25f:mode==3?0.f:1.f;
        const float t=std::exp(-density*distance);
        const float phaseRatio=mode==5?(.75f/std::pow(2.25f,1.5f)):1.f;
        const float expected=2*t+(mode==4?0.f:.8f*(1-t)*phaseRatio);
        auto pixels=f.renderResource(program,[&](const auto& p){
            auto tex=[&](const char* n,bgfx::TextureHandle h){auto b=p.getTextureBinding(n);CHECK(b!=ayt::shader::InvalidBinding);p.setTexture(p.getTextureStage(b),b,toShaderTexture(h));};
            tex("sceneColor",scene);tex("worldPosition",world);tex("geometryCoverage",sky);tex("shadowAtlas",mode==4?dark:lit);
            auto identity=ayt::math::Float4x4::identity();
            for(const char* n:{"fogInverseViewProjection","fogView","fogShadow0","fogShadow1","fogShadow2"})p.setUniform(p.getUniformBinding(n),&identity,64);
            auto u=[&](const char* n,Pixel v){auto b=p.getUniformBinding(n);CHECK(b!=ayt::shader::InvalidBinding);p.setUniform(b,v.data(),16);};
            u("fogCamera",{0,0,0,1});u("fogMedium",{density,.8f,mode==5?.5f:0.f,100});u("fogOptions",{32,0,mode==2||mode==3?1.f:0.f,0});
            u("fogCenter",{0,0,mode==3?3.f:0.f,0});u("fogExtent",{10,10,.25f,0});u("fogTint",{1,1,1,0});
            u("fogLightPosition",{0,0,0,0});u("fogLightDirection",{0,0,1,0});u("fogLightColor",{12.5663706f,12.5663706f,12.5663706f,0});u("fogSpot",{0,0,0,0});
            for(const char* n:{"fogShadowRect0","fogShadowRect1","fogShadowRect2"})u(n,{0,0,1,1});
            u("fogShadowParams",{0,0,0,mode==4?1.f:0.f});
        });
        checkPixels(pixels,{expected,expected,expected,1});
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(ssr_production_shader_preserves_sky_rough_surfaces_and_misses) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture f; CHECK(f.initialize(backend));
    if (!f.adapter.isInitialized()) return;
    auto scene = f.input({2.0f, 0.25f, 0.5f, 1.0f});
    auto world = f.input({0,0,2,1}); auto coverage = f.input({0,0,0,1});
    auto rough = f.input({0.5f,0.5f,0,1}); auto smooth = f.input({0.5f,0.5f,0,0.1f});
    auto sky = f.input({0,0,0,0}); auto lut = f.input({1,0,0,1});
    std::array<uint8_t,24> black{};
    auto cube = bgfx::createTextureCube(1,false,1,bgfx::TextureFormat::RGBA8,0,bgfx::copy(black.data(),24));
    CHECK(bgfx::isValid(cube)); if(!bgfx::isValid(cube))return; f.textures.push_back(cube);
    auto program = f.pool.acquire(ayt::render::kSsrPhoskiaSource, ayt::render::kSsrCacheKey);
    CHECK(program.isValid());
    for(int mode=0;mode<3;++mode) {
        auto pixels=f.renderResource(program,[&](const auto& p){
            auto tex=[&](const char* n,bgfx::TextureHandle t){auto b=p.getTextureBinding(n);CHECK(b!=ayt::shader::InvalidBinding);p.setTexture(p.getTextureStage(b),b,toShaderTexture(t));};
            tex("sceneColor",scene);tex("worldPosition",world);tex("geometryCoverage",mode==0?sky:coverage);
            tex("normalRoughness",mode==1?rough:smooth);tex("albedoMetallic",scene);tex("ssaoTexture",sky);
            tex("prefilteredSpecularCube",cube);tex("brdfLut",lut);
            auto identity=ayt::math::Float4x4::identity();
            p.setUniform(p.getUniformBinding("ssrViewMatrix"),&identity,64);
            p.setUniform(p.getUniformBinding("ssrProjectionMatrix"),&identity,64);
            auto u=[&](const char* n,Pixel v){p.setUniform(p.getUniformBinding(n),v.data(),16);};
            u("cameraPosition",{0,0,0,0});u("traceParams",{1,30,0.2f,48});
            u("materialParams",{0,0.6f,0,0});u("iblParams",{1,0,0,0});u("ssaoParams",{0,0.25f,0.25f,0});
        });
        checkPixels(pixels,{2.0f,0.25f,0.5f,1});
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(ssr_trace_hits_a_different_surface_and_replaces_ibl_energy) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture f; CHECK(f.initialize(backend)); if(!f.adapter.isInitialized())return;
    for (float shift : {0.0f, 5.0f}) {
    auto probeView=ayt::math::Float4x4::identity(); probeView.row[0].w=-shift;
    float probeUpload[16]; toBgfxColumnMajor(probeView,probeUpload);
    checkPixels(f.render("uniform mat4 ssrProbeView", "return ssrProbeView * vec4(5.0,0.0,0.0,1.0)",
        [&](const auto& p){p.setUniform(p.getUniformBinding("ssrProbeView"),probeUpload,64);}),{5-shift,0,0,1});
    std::array<Pixel,16> positions{},normals{};
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
        float px=(x+0.5f)*0.5f-1.0f,py=1.0f-(y+0.5f)*0.5f,z=x==0?2.0f:2.55f;
        positions[y*4+x]={px+shift,py,z,1};
        if(x==0) {
            float l=std::sqrt(px*px+py*py+z*z);
            float nx=px/l-0.8f,ny=py/l,nz=z/l-0.6f;
            float nl=std::sqrt(nx*nx+ny*ny+nz*nz);
            normals[y*4+x]={nx/nl*0.5f+0.5f,ny/nl*0.5f+0.5f,nz/nl*0.5f+0.5f,0.05f};
        } else normals[y*4+x]={0,0.5f,0.5f,0.05f};
    }
    auto grid=[&](const auto& data){auto t=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,
        BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(data.data(),sizeof(data)));f.textures.push_back(t);return t;};
    auto world=grid(positions),normal=grid(normals),scene=f.input({2,2,2,1}),surface=f.input({0,0,0,1}),albedo=f.input({1,1,1,1}),lut=f.input({1,0,0,1});
    std::array<uint8_t,24> black{};auto cube=bgfx::createTextureCube(1,false,1,bgfx::TextureFormat::RGBA8,0,bgfx::copy(black.data(),24));f.textures.push_back(cube);
    auto p=f.pool.acquire(ayt::render::kSsrPhoskiaSource,ayt::render::kSsrCacheKey);
    std::vector<Pixel> confidence;
    for(int debug : {1,0}) {
        auto pixels=f.renderResource(p,[&](const auto& shader){
            auto tex=[&](const char* n,bgfx::TextureHandle t){auto b=shader.getTextureBinding(n);shader.setTexture(shader.getTextureStage(b),b,toShaderTexture(t));};
            tex("sceneColor",scene);tex("worldPosition",world);tex("normalRoughness",normal);tex("geometryCoverage",surface);
            tex("albedoMetallic",albedo);tex("ssaoTexture",surface);tex("brdfLut",lut);tex("prefilteredSpecularCube",cube);
            auto identity=ayt::math::Float4x4::identity();
            auto view=identity; view.row[0].w=-shift;
            float viewUpload[16]; toBgfxColumnMajor(view,viewUpload);
            shader.setUniform(shader.getUniformBinding("ssrViewMatrix"),viewUpload,64);
            shader.setUniform(shader.getUniformBinding("ssrProjectionMatrix"),&identity,64);
            auto u=[&](const char* n,Pixel v){shader.setUniform(shader.getUniformBinding(n),v.data(),16);};
            u("cameraPosition",{shift,0,0,0});u("traceParams",{1,3,0.02f,48});u("materialParams",{0,0.6f,float(debug),0});u("iblParams",{1,0,0,0});u("ssaoParams",{0,0.25f,0.25f,0});
        });
        CHECK(pixels.size()==16); if(pixels.size()!=16)return;
        if(debug==1) {confidence=pixels;CHECK(std::any_of(pixels.begin(),pixels.end(),[](auto v){return v[0]>0.1f;}));}
        else for(size_t i=0;i<pixels.size();++i) {
            std::cerr << "[SSR hit] pixel=" << i << " confidence=" << confidence[i][0] << " composite=" << pixels[i][0] << '\n';
            CHECK(std::isfinite(pixels[i][0]));
            CHECK(std::abs(pixels[i][0]-(2.0f+2.0f*confidence[i][0]))<0.0001f);
        }
    }
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(phoskia_release_package_relocation_and_repeated_gpu_lifetimes_preserve_pixels) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    const auto root=std::filesystem::temp_directory_path()/("phoskia_release_stress_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {std::filesystem::path path; ~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{root};
    const std::string source="struct S {vec3 tint; float gain;} uniformblock Data {S surface;} "
        "fn accumulate(n:int):float {let r=0.0; for(i in range(n,8)){r=r+0.25;} return r;} "
        "fn lookup(t:sampler2D,uv:vec2):vec4 {return sample(t,uv);} material Probe {texture2d color; uniform int count; "
        "vertex {in p:position; return vec4(p,1.0)} fragment {return lookup(color,vec2(0.5))*vec4(Data.surface.tint*(Data.surface.gain+accumulate(count)),1.0);}}";
    const std::string compute="struct S {vec3 tint; float gain;} [numthreads(4,4,1)] compute Probe {"
        "storage seed:structuredbuffer<S> binding 0; storage image:rwimage2d<vec4> binding 1; let s=seed[0]; "
        "let r=0.0; for(i in range(int(s.gain),8)){r=r+0.25;} imageStore(image,ivec2(thread_id.xy),vec4(s.tint*r,1.0));}";
    ayt::shader::ShaderBuildManifest manifest;
    manifest.target={AY_SHADER_SHADERC_HINT,"windows","s_5_0",{AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT}};
    ayt::shader::ShaderBuildJob a,b; a.name="surface";a.variant="default";a.sourceText=source;
    b.name="kernel";b.variant="default";b.sourceText=compute;manifest.jobs={a,b};
    ayt::shader::ShaderBuildSession session;ayt::shader::ShaderBuildReport report,loaded;
    session.build(manifest,report); CHECK(report.success);if(!report.success)return;
    std::vector<std::string> errors;
    CHECK(ayt::shader::writeShaderBuildPackage(report,(root/"original").string(),errors));
    std::filesystem::rename(root/"original",root/"relocated");
    CHECK(ayt::shader::readShaderBuildPackage((root/"relocated").string(),loaded));
    CHECK(ayt::shader::validateShaderBuildCoverage(manifest,loaded,errors));
    PhoskiaGpuFixture fixture;const bool initialized=fixture.initialize(backend);CHECK(initialized);if(!initialized)return;
    fixture.pool.setRuntimeCompilationEnabled(false);fixture.pool.setShadercExecutable("missing-release-compiler.exe");
    fixture.pool.setBgfxIncludeDirs({});
    const auto tex=fixture.input({1,1,1,1});CHECK(bgfx::isValid(tex));if(!bgfx::isValid(tex))return;
    for(int frame=0;frame<3;++frame)fixture.adapter.endFrame();
    const auto programs=bgfx::getStats()->numPrograms,uniforms=bgfx::getStats()->numUniforms;
    const auto buffers=bgfx::getStats()->numDynamicVertexBuffers,textures=bgfx::getStats()->numTextures;
    std::vector<double> timings;
    for(int cycle=0;cycle<16;++cycle) {
        const auto start=std::chrono::steady_clock::now();
        CHECK(fixture.pool.installPrecompiled(loaded,errors));
        auto surface=fixture.pool.acquire(source);auto kernel=fixture.pool.acquire(compute);
        CHECK(surface.isValid() && kernel.isValid());if(!surface.isValid() || !kernel.isValid())return;
        const float data[4]={.2f,.4f,.6f,.5f};const int count=2;
        checkPixels(fixture.renderResource(surface,[&](const auto& shader){
            shader.setUniformBlock(shader.getUniformBlockBinding("Data"),data,sizeof(data));
            shader.setUniform(shader.getUniformBinding("count"),&count,sizeof(count));
            shader.setTexture(0,shader.getTextureBinding("color"),toShaderTexture(tex));
        }),{.2f,.4f,.6f,1});
        const float seed[4]={.2f,.4f,.6f,4};
        auto buffer=fixture.pool.createStorageBuffer(kernel,kernel.getStorageBufferBinding("seed"),1,seed,sizeof(seed));
        auto image=fixture.pool.createStorageImage(4,4); CHECK(buffer.isValid() && image.isValid());
        if(!buffer.isValid() || !image.isValid())return;
        std::string error;
        CHECK(fixture.pool.dispatch(kernel,{0,1,1,1},{{kernel.getStorageBufferBinding("seed"),buffer}},{{kernel.getStorageImageBinding("image"),image}},&error));
        checkPixels(fixture.readImage(ayt::shader::detail::ComputeResourceAccess::borrowImageTexture(fixture.pool,image)),{.2f,.4f,.6f,1});
        CHECK(fixture.pool.releaseStorageBuffer(buffer));CHECK(fixture.pool.releaseStorageImage(image));
        CHECK(fixture.pool.cacheStats().sourceMisses==0 && fixture.pool.cacheStats().binaryMisses==0);
        fixture.pool.shutdown();CHECK(!surface.isValid() && !kernel.isValid());
        for(int frame=0;frame<3;++frame)fixture.adapter.endFrame();
        CHECK(bgfx::getStats()->numPrograms==programs);CHECK(bgfx::getStats()->numUniforms==uniforms);
        CHECK(bgfx::getStats()->numDynamicVertexBuffers==buffers);CHECK(bgfx::getStats()->numTextures==textures);
        timings.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    }
    std::sort(timings.begin(),timings.end());
    std::cout<<"[PhoskiaBaseline] {\"metric\":\"gpuLifecycleRoundTrip\",\"backend\":\""<<backend<<"\",\"samples\":16,\"medianMs\":"
        <<timings[8]<<",\"p95Ms\":"<<timings[14]<<",\"maxMs\":"<<timings.back()<<"}\n";
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_runtime_loop_limits_are_clamped_once_and_do_not_depend_on_index_writes) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture fixture; const bool initialized=fixture.initialize(backend); CHECK(initialized); if(!initialized) return;
    const std::string source="fn sum(n:int):float {let result=0.0; for(i in range(n,8)) {result=result+float(i)+1.0; n=0; i=-1;} "
        "for(i in range(n,8)) {result=result+100.0;} return result;} material Probe {uniform int count; "
        "vertex {in p:position; return vec4(p,1.0)} fragment {return vec4(sum(count));}}";
    for(const int count:{-1,0,3,8,99}) {
        const auto actualCount=(std::min)((std::max)(count,0),8);
        const float expected=static_cast<float>(actualCount*(actualCount+1)/2);
        checkPixels(fixture.renderSource(source,[&](const auto& program) {program.setUniform(program.getUniformBinding("count"),&count,sizeof(count));}),{expected,expected,expected,expected});
    }
    auto kernel=fixture.pool.acquire("[numthreads(4,4,1)] compute M {storage input:structuredbuffer<int> binding 0; storage image:rwimage2d<vec4> binding 1; "
        "let id=thread_id; let index=int(id.x)+int(id.y)*4; let result=0.0; for(i in range(input[index],8)) {result=result+1.0;} imageStore(image,ivec2(id.xy),vec4(result));}");
    CHECK(kernel.isValid()); if(!kernel.isValid()) {for(const auto& e:fixture.pool.lastCompileErrors())std::cerr<<e<<'\n';return;}
    std::array<int,16> counts; counts.fill(5);
    auto input=fixture.pool.createStorageBuffer("int",16,counts.data(),sizeof(counts)); CHECK(input.isValid());
    auto image=fixture.pool.createStorageImage(4,4); CHECK(image.isValid());
    std::string error; CHECK(fixture.pool.dispatch(kernel,{0,1,1,1},{{kernel.getStorageBufferBinding("input"),input}},{{kernel.getStorageImageBinding("image"),image}},&error));
    checkPixels(fixture.readImage(ayt::shader::detail::ComputeResourceAccess::borrowImageTexture(fixture.pool,image)),{5,5,5,5});
    CHECK(fixture.pool.releaseStorageBuffer(input)); CHECK(fixture.pool.releaseStorageImage(image));
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_gpu_struct_layout_upload_and_storage_read_write_match_pixels) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture fixture; const bool initialized=fixture.initialize(backend); CHECK(initialized); if(!initialized) return;
    std::array<float,12> block{};
    block[0]=.1f; block[4]=.2f; block[5]=.4f; block[6]=.6f; block[7]=.5f; block[8]=.3f;
    checkPixels(fixture.renderSource("struct Surface { vec3 tint; float gain; } uniformblock Params { float prefix; Surface surface; float suffix; } "
        "material M { vertex {in p:position; return vec4(p,1.0)} fragment {let s=Params.surface; return vec4(s.tint*s.gain,Params.prefix+Params.suffix);} }",
        [&](const auto& program) {
            const auto binding=program.getUniformBlockBinding("Params");
            CHECK(program.getUniformBlockSize(binding)==48);
            CHECK(program.getUniformBlockFieldOffset(binding,"surface.gain")==28);
            program.setUniformBlock(binding,block.data(),sizeof(block));
        }),{.1f,.2f,.3f,.4f});
    const std::string source="struct Surface { vec3 tint; float gain; int index; bool enabled; } "
        "[numthreads(4,4,1)] compute Probe { storage input:structuredbuffer<Surface> binding 0; storage values:rwstructuredbuffer<Surface> binding 1; "
        "storage image:rwimage2d<vec4> binding 2; let id=thread_id; let item=int(id.x)+int(id.y)*4; values[item]=input[item]; values[item].gain=0.5; "
        "let s=values[item]; imageStore(image,ivec2(id.xy),vec4(s.tint*s.gain,float(s.index))); }";
    auto kernel=fixture.pool.acquire(source); CHECK(kernel.isValid());
    if (!kernel.isValid()) {for(const auto& e:fixture.pool.lastCompileErrors())std::cerr<<e<<'\n';return;}
    const auto binding=kernel.getStorageBufferBinding("values");
    CHECK(kernel.getStorageBufferElementStride(binding)==32);
    size_t offset=0,size=0; CHECK(kernel.getStorageBufferFieldLayout(binding,"index",offset,size)); CHECK(offset==16 && size==4);
    std::array<uint8_t,512> bytes{}; const float tint[4]={.2f,.4f,.6f,1}; const int index=1;
    for(size_t item=0;item<16;++item) {std::memcpy(bytes.data()+32*item,tint,16); std::memcpy(bytes.data()+32*item+16,&index,4); bytes[32*item+20]=1;}
    const auto inputBinding=kernel.getStorageBufferBinding("input");
    auto input=fixture.pool.createStorageBuffer(kernel,inputBinding,16,bytes.data(),bytes.size()); CHECK(input.isValid());
    auto values=fixture.pool.createStorageBuffer(kernel,binding,16); CHECK(values.isValid());
    CHECK(!fixture.pool.createStorageBuffer(kernel,binding,16,bytes.data(),bytes.size()).isValid());
    CHECK(!fixture.pool.createStorageBuffer(kernel,inputBinding,16,bytes.data(),bytes.size()-1).isValid());
    auto image=fixture.pool.createStorageImage(4,4); CHECK(image.isValid());
    std::string error;
    auto wrong=fixture.pool.acquire("struct Surface { vec4 tint; float gain; int index; bool enabled; } compute Wrong {storage values:rwstructuredbuffer<Surface> binding 0; values[0]=Surface(vec4(1.0),1.0,1,true);}");
    CHECK(wrong.isValid());
    CHECK(!fixture.pool.dispatch(wrong,{0,1,1,1},{{wrong.getStorageBufferBinding("values"),values}},&error));
    CHECK(!fixture.pool.dispatch(kernel,{0,1,1,1},{{inputBinding,input},{binding,input}},{{kernel.getStorageImageBinding("image"),image}},&error));
    bgfx::setViewMode(0,bgfx::ViewMode::Sequential);
    CHECK(fixture.pool.dispatch(kernel,{0,1,1,1},{{inputBinding,input},{binding,values}},{{kernel.getStorageImageBinding("image"),image}},&error));
    checkPixels(fixture.readImage(ayt::shader::detail::ComputeResourceAccess::borrowImageTexture(fixture.pool,image)),{.1f,.2f,.3f,1});
    CHECK(fixture.pool.releaseStorageBuffer(input)); CHECK(fixture.pool.releaseStorageBuffer(values)); CHECK(fixture.pool.releaseStorageImage(image));
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_explicit_texture_helpers_read_real_resources) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture fixture; const bool initialized = fixture.initialize(backend);
    CHECK(initialized); if (!initialized) return;
    const Pixel color = {.2f,.4f,.6f,.8f};
    const auto texture = fixture.input(color); CHECK(bgfx::isValid(texture));
    for (const auto& sampling : {"sample(t,uv)", "sampleLod(t,uv,0.0)"}) {
        const auto source = std::string("fn outer(t:sampler2D,uv:vec2):vec4 { return inner(t,uv); } ") +
            "fn inner(t:sampler2D,uv:vec2):vec4 { return " + sampling + "; } material Probe { texture2d tex; "
            "vertex { in p:position; return vec4(p,1.0) } fragment { return outer(tex,vec2(0.5)); } }";
        checkPixels(fixture.renderSource(source,[&](const auto& program) {
            program.setTexture(0, program.getTextureBinding("tex"), toShaderTexture(texture));
        }),color);
    }
    std::array<Pixel,6> faces; faces.fill(color);
    const auto cube = bgfx::createTextureCube(1,false,1,bgfx::TextureFormat::RGBA32F,BGFX_SAMPLER_POINT,bgfx::copy(faces.data(),sizeof(faces)));
    CHECK(bgfx::isValid(cube)); if (bgfx::isValid(cube)) fixture.textures.push_back(cube);
    const std::string cubeSource = "fn read(t:samplerCube,direction:vec3):vec4 { return sampleLod(t,direction,0.0); } "
        "material Probe { texturecube tex; vertex { in p:position; return vec4(p,1.0) } "
        "fragment { return read(tex,vec3(1.0,0.0,0.0)); } }";
    checkPixels(fixture.renderSource(cubeSource,[&](const auto& program) {
        program.setTexture(0, program.getTextureBinding("tex"),toShaderTexture(cube));
    }),color);
    const std::string vertexSource = "fn read(t:sampler2D,uv:vec2):vec4 { return sampleLod(t,uv,0.0); } "
        "material Probe { texture2d tex; vertex { in p:position; out tint:color; tint=read(tex,vec2(0.5)); return vec4(p,1.0) } "
        "fragment { in tint:color; return tint; } }";
    checkPixels(fixture.renderSource(vertexSource,[&](const auto& program) {
        program.setTexture(0, program.getTextureBinding("tex"),toShaderTexture(texture));
    }),color);
#else
    SKIP_TEST("Requires Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_compute_atomics_barriers_and_image_dispatch_read_back) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    auto count = fixture.pool.acquire("[numthreads(4,1,1)] compute Count { shared uint sum[1]; "
        "storage result : rwstructuredbuffer<uint> binding 0; "
        "if (thread_id.x == uint(0)) { sum[0] = uint(0); } barrier(); "
        "atomicAdd(sum[0],uint(1)); barrier(); result[thread_id.x] = sum[0] + dispatch_id.x; }");
    auto imageKernel = fixture.pool.acquire("[numthreads(4,4,1)] compute Output { storage counts : structuredbuffer<uint> binding 0; "
        "storage target : rwimage2d<vec4> binding 1; "
        "let color = float(counts[thread_id.x + uint(4)]) * 0.0625; imageStore(target,ivec2(int(thread_id.x),int(thread_id.y)),vec4(color,0.5,0.75,1.0)); }");
    CHECK(count.isValid()); CHECK(imageKernel.isValid());
    if (!count.isValid() || !imageKernel.isValid()) { for(const auto& error:fixture.pool.compileErrorsSnapshot()) std::cerr<<error<<'\n'; return; }
    auto buffer = fixture.pool.createStorageBuffer("uint",8);
    auto image = fixture.pool.createStorageImage(4,4);
    CHECK(buffer.isValid()); CHECK(image.isValid());
    if(!buffer.isValid() || !image.isValid()) return;
    std::string error;
    CHECK(fixture.pool.dispatch(count,{0,2,1,1},{{count.getStorageBufferBinding("result"),buffer}},&error));
    CHECK(fixture.pool.dispatch(imageKernel,{1,1,1,1},{{imageKernel.getStorageBufferBinding("counts"),buffer}},
        {{imageKernel.getStorageImageBinding("target"),image}},&error));
    checkPixels(fixture.readImage(ayt::shader::detail::ComputeResourceAccess::borrowImageTexture(fixture.pool,image)),{.375f,.5f,.75f,1});
    auto global = fixture.pool.acquire("[numthreads(4,1,1)] compute Global { storage result : rwstructuredbuffer<uint> binding 0; "
        "if (thread_id.x == uint(0)) { result[0] = uint(0); } memoryBarrierBuffer(); "
        "atomicAdd(result[0],uint(1)); memoryBarrierBuffer(); let sum = result[0]; "
        "result[thread_id.x + uint(4)] = sum; }");
    CHECK(global.isValid());
    CHECK(fixture.pool.dispatch(global,{0,1,1,1},{{global.getStorageBufferBinding("result"),buffer}},&error));
    CHECK(fixture.pool.dispatch(imageKernel,{1,1,1,1},{{imageKernel.getStorageBufferBinding("counts"),buffer}},
        {{imageKernel.getStorageImageBinding("target"),image}},&error));
    checkPixels(fixture.readImage(ayt::shader::detail::ComputeResourceAccess::borrowImageTexture(fixture.pool,image)),{.25f,.5f,.75f,1});
    const auto stale = image;
    CHECK(fixture.pool.releaseStorageImage(image));
    CHECK(!fixture.pool.dispatch(imageKernel,{1,1,1,1},{{imageKernel.getStorageBufferBinding("counts"),buffer}},
        {{imageKernel.getStorageImageBinding("target"),stale}},&error));
    CHECK(fixture.pool.releaseStorageBuffer(buffer));
    fixture.pool.release(count);fixture.pool.release(imageKernel);fixture.pool.release(global);
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_compute_dispatch_writes_buffer_consumed_by_graphics) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    auto kernel = fixture.pool.acquire("[numthreads(4,1,1)] compute Probe { storage colors : rwstructuredbuffer<vec4> binding 0; "
        "let index = int(thread_id.x); if (index < 3) { colors[index] = vec4(0.2,0.4,0.6,1.0); } }");
    CHECK(kernel.isValid());
    if (!kernel.isValid()) { for (const auto& error : fixture.pool.compileErrorsSnapshot()) std::cerr << error << '\n'; return; }
    auto buffer = fixture.pool.createStorageBuffer("vec4",3);
    CHECK(buffer.isValid());
    if (!buffer.isValid()) return;
    std::string error;
    const auto binding = kernel.getStorageBufferBinding("colors");
    CHECK(binding != ayt::shader::InvalidBinding);
    CHECK(kernel.getStorageImageBinding("colors") == ayt::shader::InvalidBinding);
    CHECK(!fixture.pool.createStorageBuffer("vec3",3).isValid());
    CHECK(!fixture.pool.createStorageBuffer("uint",0).isValid());
    const uint32_t seed = 7;
    CHECK(!fixture.pool.createStorageBuffer("uint",3,&seed,sizeof(seed)).isValid());
    const Pixel seedColors[3]={{.2f,.4f,.6f,1},{.2f,.4f,.6f,1},{.2f,.4f,.6f,1}};
    auto initializedReadonly=fixture.pool.createStorageBuffer("vec4",3,seedColors,sizeof(seedColors));
    CHECK(initializedReadonly.isValid());
    CHECK(!fixture.pool.dispatch(kernel,{0,1,1,1},{{binding,initializedReadonly}},&error));
    CHECK(fixture.pool.releaseStorageBuffer(initializedReadonly));
    CHECK(!fixture.pool.createStorageImage(0,4).isValid());
    CHECK(!fixture.pool.dispatch(kernel,{UINT16_MAX,1,1,1},{{binding,buffer}},&error));
    CHECK(!fixture.pool.dispatch(kernel,{0,65536,1,1},{{binding,buffer}},&error));
    CHECK(!fixture.pool.dispatch(kernel,{0,1,1,1},{{binding,buffer},{binding,buffer}},&error));
    CHECK(!fixture.pool.dispatch(kernel,{0,0,1,1},{{binding,buffer}},&error));
    CHECK(!error.empty());
    CHECK(!fixture.pool.dispatch(kernel,{0,1,1,1},{},&error));
    bool foreignThreadAccepted = true;
    std::thread worker([&] { foreignThreadAccepted = fixture.pool.dispatch(kernel,{0,1,1,1},{{binding,buffer}}); });
    worker.join();
    CHECK(!foreignThreadAccepted);
    ayt::shader::ShaderResourcePool other;
    auto foreignBuffer = buffer;
    CHECK(!other.releaseStorageBuffer(foreignBuffer));
    CHECK(foreignBuffer.id == buffer.id);
    other.release(kernel); CHECK(kernel.isValid());
    CHECK(!other.dispatch(kernel,{0,1,1,1},{{binding,buffer}},&error));
    bgfx::setViewMode(0,bgfx::ViewMode::Sequential);
    CHECK(fixture.pool.dispatch(kernel,{0,1,1,1},{{binding,buffer}},&error));
    auto native = ayt::shader::detail::ComputeResourceAccess::borrowVertexBuffer(fixture.pool,buffer);
    CHECK(bgfx::isValid(native));
    checkPixels(fixture.renderSource("material Probe { vertex { in p : position; in c : color; out tint : color; "
        "tint = c; return vec4(p,1.0); } fragment { in tint : color; return tint; } }",[&](const auto&) {
        bgfx::setVertexBuffer(1,native,0,3);
    }),{.2f,.4f,.6f,1});
    const auto stale = buffer;
    CHECK(fixture.pool.releaseStorageBuffer(buffer));
    CHECK(!fixture.pool.dispatch(kernel,{0,1,1,1},{{binding,stale}},&error));
    CHECK(!bgfx::isValid(ayt::shader::detail::ComputeResourceAccess::borrowVertexBuffer(fixture.pool,stale)));
    fixture.pool.release(kernel);
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_modules_recompile_changed_content_on_device) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("phoskia_module_gpu_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path,ec); } } cleanup{root};
    const auto sourcePath = (root/"probe.phoskia").string();
    const auto modulePath = (root/"math.phoskia").string();
    CHECK(ayt::io::File::writeAllText(sourcePath,
        "include \"math.phoskia\";\nmaterial Probe { vertex { in p : position; return vec4(p,1.0); } "
        "fragment { return vec4(shade(Surface(vec3(0.1,0.2,0.3))),1.0); } }"));
    CHECK(ayt::io::File::writeAllText(modulePath,
        "struct Surface { vec3 tint; } fn shade(s:Surface):vec3 { return s.tint * 2.0; }\n"));
    const auto stamp = fs::last_write_time(modulePath);
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    auto first = fixture.pool.compileFromFile(sourcePath);
    checkPixels(fixture.renderResource(first), {.2f,.4f,.6f,1});
    CHECK(ayt::io::File::writeAllText(modulePath,
        "struct Surface { vec3 tint; } fn shade(s:Surface):vec3 { return s.tint * 3.0; }\n"));
    fs::last_write_time(modulePath,stamp);
    auto second = fixture.pool.compileFromFile(sourcePath);
    checkPixels(fixture.renderResource(second), {.3f,.6f,.9f,1});
    CHECK(fixture.pool.cacheStats().sourceMisses == 2);
    CHECK(fixture.pool.cacheStats().binaryMisses == 2);
    fixture.pool.release(first);
    fixture.pool.release(second);
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_offline_package_prewarm_matches_device_without_shaderc) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("phoskia_package_gpu_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path,ec); } } cleanup{root};
    const auto sourcePath = (root/"probe.phoskia").string();
    CHECK(ayt::io::File::writeAllText(sourcePath,
        "uniformblock Data { vec3 color; float gain; } material Probe { "
        "vertex { in p : position; return vec4(p, 1.0) } fragment { return vec4(Data.color * Data.gain, 1.0) } }"));
    ayt::shader::ShaderBuildManifest manifest;
    manifest.target = {AY_SHADER_SHADERC_HINT,"windows","s_5_0",{AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT}};
    manifest.jobs = {{"probe","default",sourcePath,{}}};
    ayt::shader::ShaderBuildSession session;
    ayt::shader::ShaderBuildReport built, loaded;
    session.build(manifest,built);
    CHECK(built.success);
    if (!built.success) return;
    std::vector<std::string> errors;
    CHECK(ayt::shader::writeShaderBuildPackage(built,(root/"package").string(),errors));
    const bool packageValid = ayt::shader::readShaderBuildPackage((root/"package").string(),loaded);
    CHECK(packageValid);
    if (!packageValid) return;
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    fixture.pool.setShadercExecutable("nonexistent/offline-prewarm-must-not-compile.exe");
    std::vector<ayt::shader::ShaderResource> handles;
    CHECK(ayt::shader::prewarmShaderBuild(fixture.pool,loaded,handles,errors));
    CHECK(handles.size() == 1);
    if (handles.size() != 1) return;
    const float payload[4] = {.2f,.4f,.6f,.5f};
    checkPixels(fixture.renderResource(handles[0], [&](const auto& program) {
        program.setUniformBlock(program.getUniformBlockBinding("Data"),payload,sizeof(payload));
    }), {.1f,.2f,.3f,1});
    CHECK(fixture.pool.cacheStats().sourceMisses == 0);
    fixture.pool.release(handles[0]);
    handles.clear();
    CHECK(fixture.pool.installPrecompiled(loaded,errors));
    fixture.pool.setRuntimeCompilationEnabled(false);
    const auto normal = fixture.pool.compileFromFile(sourcePath);
    CHECK(normal.isValid());
    checkPixels(fixture.renderResource(normal, [&](const auto& program) {
        program.setUniformBlock(program.getUniformBlockBinding("Data"),payload,sizeof(payload));
    }), {.1f,.2f,.3f,1});
    CHECK(fixture.pool.cacheStats().binaryMisses == 0);
    CHECK(fixture.pool.cacheStats().sourceMisses == 0);
    auto mismatched = loaded;
    mismatched.platform = "linux"; mismatched.profile = "430";
    fixture.pool.setPlatform("linux"); fixture.pool.setGLSLProfile("430");
    CHECK(!ayt::shader::prewarmShaderBuild(fixture.pool,mismatched,handles,errors));
    CHECK(handles.empty()); // Explicit pool settings cannot disguise real D3D.
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_integer_boolean_and_mixed_block_uploads_match_gpu_values) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated AY_TAA_GPU_TEST=d3d11/d3d12");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    const int32_t signedValue = -7;
    const uint32_t unsignedValue = 4000000001u;
    for (uint32_t flag : {0u, 1u}) {
        checkPixels(fixture.render("uniform int signedValue; uniform uint unsignedValue; uniform bool enabled;",
            "let r = 0.0; let g = 0.0; let b = 0.0; if (signedValue == -7) { r = 1.0 } "
            "if (unsignedValue - uint(4000000000.0) == uint(1)) { g = 1.0 } "
            "if (enabled) { b = 1.0 } return vec4(r, g, b, 1.0);",
            [&](const auto& program) {
                program.setUniform(program.getUniformBinding("signedValue"), &signedValue, sizeof(signedValue));
                program.setUniform(program.getUniformBinding("unsignedValue"), &unsignedValue, sizeof(unsignedValue));
                program.setUniform(program.getUniformBinding("enabled"), &flag, sizeof(flag));
            }), {1, 1, static_cast<float>(flag), 1});
    }
    // std140 vec3 + float share a slot; float[2] has 16-byte stride.
    const auto source = "uniformblock Camera { vec3 position; float gain; float weights[2]; } "
        "material Probe { vertex { in p : position; return vec4(p, 1.0) } "
        "fragment { return vec4(Camera.position * Camera.gain, Camera.weights[1]) } }";
    std::array<uint8_t, 48> payload{};
    const float position[3] = {.2f, .3f, .4f};
    const float gain = 2.0f, weight = .75f;
    std::memcpy(payload.data(), position, sizeof(position));
    std::memcpy(payload.data() + 12, &gain, sizeof(gain));
    std::memcpy(payload.data() + 32, &weight, sizeof(weight));
    checkPixels(fixture.renderSource(source, [&](const auto& program) {
        auto block = program.getUniformBlockBinding("Camera");
        CHECK(block != ayt::shader::InvalidBinding);
        CHECK(program.getUniformBlockSize(block) == 48);
        CHECK(program.getUniformBlockFieldOffset(block, "weights") == 16);
        program.setUniformBlock(block, payload.data(), payload.size());
    }), {.4f, .6f, .8f, .75f});
    const int32_t signedVector[4] = {-3, 9, 13, 0};
    const uint32_t maxWord = UINT32_MAX;
    checkPixels(fixture.render("uniform uint maxWord; uniform bool maxFlag;",
        "let value = 0.0; if (maxWord == uint(0) - uint(1) && maxFlag) { value = 1.0 } return vec4(value);",
        [&](const auto& program) {
            program.setUniform(program.getUniformBinding("maxWord"), &maxWord, sizeof(maxWord));
            program.setUniform(program.getUniformBinding("maxFlag"), &maxWord, sizeof(maxWord));
        }), {1,1,1,1});
    const uint32_t unsignedVector[4] = {4000000001u, 23u, 41u, 0};
    checkPixels(fixture.render("uniform ivec3 signedVector; uniform uvec3 unsignedVector;",
        "return vec4(float(signedVector.x), float(signedVector.y), float(unsignedVector.y), float(unsignedVector.z));",
        [&](const auto& program) {
            program.setUniform(program.getUniformBinding("signedVector"), signedVector, sizeof(signedVector));
            program.setUniform(program.getUniformBinding("unsignedVector"), unsignedVector, sizeof(unsignedVector));
        }), {-3, 9, 23, 41});
    const int32_t integerSlots[8] = {-3,0,0,0, 9,0,0,0};
    const uint32_t enabled = 1;
    checkPixels(fixture.render("uniform int values[2]; property enabled = true;",
        "let alpha = 0.0; if (enabled) { alpha = 1.0 } return vec4(float(values[0]), float(values[1]), 0.0, alpha);",
        [&](const auto& program) {
            program.setUniform(program.getUniformBinding("values"), integerSlots, sizeof(integerSlots));
            program.setUniform(program.getUniformBinding("enabled"), &enabled, sizeof(enabled));
        }), {-3,9,0,1});
    const auto matrixSource = "uniformblock FrameData { mat3 basis; uint marker; float gain[1]; } "
        "uniformblock OtherData { float marker; } material Probe { "
        "vertex { in p : position; return vec4(p, 1.0) } fragment { "
        "let alpha = OtherData.marker; if (FrameData.marker == uint(123)) { alpha = alpha + 0.1 } "
        "return vec4(FrameData.basis * vec3(0.1, 0.2, 0.3) * FrameData.gain[0], alpha) } }";
    std::array<uint8_t, 80> matrixPayload{};
    const float matrixSlots[12] = {2,0,0,0, 0,3,0,0, 0,0,4,0};
    const uint32_t marker = 123;
    const float unitGain = 1, otherMarker = .25f;
    std::memcpy(matrixPayload.data(), matrixSlots, sizeof(matrixSlots));
    std::memcpy(matrixPayload.data() + 48, &marker, sizeof(marker));
    std::memcpy(matrixPayload.data() + 64, &unitGain, sizeof(unitGain));
    std::array<uint8_t, 16> otherPayload{};
    std::memcpy(otherPayload.data(), &otherMarker, sizeof(otherMarker));
    checkPixels(fixture.renderSource(matrixSource, [&](const auto& program) {
        auto block = program.getUniformBlockBinding("FrameData");
        CHECK(program.getUniformBlockSize(block) == 80);
        CHECK(program.getUniformBlockFieldOffset(block, "gain") == 64);
        program.setUniformBlock(block, matrixPayload.data(), matrixPayload.size());
        program.setUniformBlock(program.getUniformBlockBinding("OtherData"), otherPayload.data(), otherPayload.size());
    }), {.2f, .6f, 1.2f, .35f});
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_value_structs_execute_construction_fields_and_helpers) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    const auto source = "fn scale(s:Surface, value:float):Surface { return Surface(s.tint * value, s.gain, s.enabled); } "
        "fn shade(s:Surface):vec3 { if (s.enabled) { return s.tint * s.gain; } return vec3(0.0); } "
        "struct Surface { vec3 tint; float gain; bool enabled; } "
        "material Probe { vertex { in p : position; return vec4(p, 1.0); } fragment { "
        "let s = Surface(vec3(0.1, 0.2, 0.3), 2.0, true); let scaled = scale(s, 0.5); "
        "scaled.gain = 4.0; return vec4(shade(scaled), 1.0); } }";
    checkPixels(fixture.renderSource(source), {.2f,.4f,.6f,1});
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_typed_helpers_execute_returns_loops_and_forward_calls) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    const auto source = "fn helper(value:float):float { if (value < 0.0) { return 0.125; } "
        "let sum = 0.0; for (i in range(4)) { sum = sum + value; } return twice(sum); } "
        "fn twice(value:float):float { return value * 2.0; } "
        "fn tint(value:vec3, gain:float):vec3 { return value * gain; } "
        "fn project(p:vec3):vec4 { return vec4(p, 1.0); } "
        "material Probe { vertex { in p : position; return project(p); } fragment { "
        "let rgb = tint(vec3(helper(-1.0), helper(0.0625), 0.25), 1.0); return vec4(rgb, 1.0); } }";
    checkPixels(fixture.renderSource(source), {.125f,.5f,.25f,1});
    checkPixels(fixture.renderSource("fn enabled(flag:bool):uint { if (flag) { return uint(123); } return uint(7); } "
        "fn branch(value:float):float { if (value > 0.0) { let local = vec3(value); } return value; } "
        "material Probe { uniform float ayt_fn_0; vertex { in p : position; return vec4(p, 1.0); } "
        "fragment { let ayt_arg_0 = 0.25; return vec4(float(enabled(true)), float(enabled(false)), branch(ayt_arg_0), 1.0); } }"), {123,7,.25f,1});
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_bounded_loops_keep_private_progress_and_scope_on_device) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    checkPixels(fixture.render("uniform float index;",
        "let sum = 0.0; let outside = 0.25; for (index in range(4)) { "
        "sum = sum + float(index); index = -1; let outside = 8.0; } "
        "return vec4(sum / 6.0, outside, 0.5, 1.0);"), {1,.25f,.5f,1});
    checkPixels(fixture.render("", "let sum = 0.0; let ayt_loop_0 = 0.5; "
        "for (i in range(4)) { for (j in range(2)) { sum = sum + float(i + j); } } "
        "return vec4(sum / 16.0, ayt_loop_0, 0.25, 1.0);"), {1,.5f,.25f,1});
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_arithmetic_branches_and_uniform_upload_match_gpu_values) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated AY_TAA_GPU_TEST=d3d11/d3d12");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    checkPixels(fixture.render("", "let a = 12.0; let b = 3.0; let c = 2.0; return vec4(a-b-c, a/b/c, a-(b-c), 1.0)"), {7, 2, 11, 1});
    for (float input : {0.0f, 1.0f, 2.0f, 3.0f}) {
        auto pixels = fixture.render("uniform float inputValue;",
            "let result = vec4(0.0); if (inputValue == 0.0) { result = vec4(0.1) } "
            "else if (inputValue >= 1.0 && inputValue <= 2.0 && inputValue != 1.5) { result = vec4(0.6) } "
            "else { result = vec4(0.9) } return result;",
            [&](const auto& program) {
                const auto binding = program.getUniformBinding("inputValue");
                CHECK(binding != ayt::shader::InvalidBinding);
                program.setUniform(binding, &input, sizeof(input));
            });
        const float expected = input == 0 ? 0.1f : input <= 2 ? 0.6f : 0.9f;
        checkPixels(pixels, {expected, expected, expected, expected});
    }
    const float matrix[16] = {2,0,0,0, 0,3,0,0, 0,0,4,0, .3f,-.2f,.1f,1};
    checkPixels(fixture.render("uniform mat4 transform;", "return transform * vec4(0.1, 0.2, 0.3, 1.0);",
        [&](const auto& program) { program.setUniform(program.getUniformBinding("transform"), matrix, sizeof(matrix)); }), {.5f,.4f,1.3f,1});
    const float scalarPad[4] = {2,0,0,0};
    checkPixels(fixture.render("uniform float scalarPad;", "return vec4(scalarPad);",
        [&](const auto& program) { program.setUniform(program.getUniformBinding("scalarPad"), scalarPad, sizeof(scalarPad)); }), {2,2,2,2});
    const float vector[4] = {.2f,.4f,.6f,.8f};
    checkPixels(fixture.render("uniform vec4 vectorValue;", "return vectorValue;",
        [&](const auto& program) { program.setUniform(program.getUniformBinding("vectorValue"), vector, sizeof(vector)); }), {.2f,.4f,.6f,.8f});
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_sampler_binding_is_authoritative_for_arbitrary_names) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated AY_TAA_GPU_TEST=d3d11/d3d12");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    auto first = fixture.input({.2f,.4f,.6f,1});
    auto second = fixture.input({.8f,.3f,.1f,1});
    CHECK(bgfx::isValid(first) && bgfx::isValid(second));
    if (!bgfx::isValid(first) || !bgfx::isValid(second)) return;
    for (const char* declarations : {"texture2d first; texture2d second;", "texture2d second; texture2d first;"}) {
        auto pixels = fixture.render(declarations,
            "return sample(first, vec2(0.5)) * 0.25 + sample(second, vec2(0.5)) * 0.75;",
            [&](const auto& program) {
                // Deliberately incorrect caller hints: compiled sampler slots
                // must win independently of name and declaration order.
                program.setTexture(7, program.getTextureBinding("first"), toShaderTexture(first));
                program.setTexture(6, program.getTextureBinding("second"), toShaderTexture(second));
            });
        checkPixels(pixels, {.65f,.325f,.225f,1});
    }
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_small_vectors_arrays_and_shadowed_uniforms_match_gpu_values) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated AY_TAA_GPU_TEST=d3d11/d3d12");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    const float scalarSlots[12] = {.1f,0,0,0, .4f,0,0,0, .7f,0,0,0};
    const float vector[3] = {.2f,.3f,.4f};
    const float gain = .25f;
    checkPixels(fixture.render("uniform float weights[3]; uniform vec3 basis; property gain = 0.5;",
        "return vec4(weights[0], weights[1], weights[2], gain + basis.x);",
        [&](const auto& program) {
            program.setUniform(program.getUniformBinding("weights"), scalarSlots, sizeof(scalarSlots));
            program.setUniform(program.getUniformBinding("basis"), vector, sizeof(vector));
            program.setUniform(program.getUniformBinding("gain"), &gain, sizeof(gain));
        }), {.1f,.4f,.7f,.45f});
    checkPixels(fixture.render("uniform float single[1];", "return vec4(single[0]);",
        [&](const auto& program) { program.setUniform(program.getUniformBinding("single"), &gain, sizeof(gain)); }), {.25f,.25f,.25f,.25f});
    checkPixels(fixture.render("uniform float value;", "let value = value + 1.0; return vec4(value);",
        [&](const auto& program) { program.setUniform(program.getUniformBinding("value"), &gain, sizeof(gain)); }), {1.25f,1.25f,1.25f,1.25f});
    checkPixels(fixture.render("uniform float value;", "let result = value; if (true) { let value = 1.0; result = value } else { result = value } return vec4(result, value, 0.0, 1.0);",
        [&](const auto& program) { program.setUniform(program.getUniformBinding("value"), &gain, sizeof(gain)); }), {1,.25f,0,1});
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_vector_constructors_preserve_components_and_matrix_uploads) {
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend || !backend[0]) SKIP_TEST("Requires isolated AY_TAA_GPU_TEST=d3d11/d3d12");
    PhoskiaGpuFixture fixture;
    const bool initialized = fixture.initialize(backend);
    CHECK(initialized);
    if (!initialized) return;
    const float color[4] = {.2f,.4f,.6f,.8f};
    const auto bindColor = [&](const auto& program) {
        program.setUniform(program.getUniformBinding("colorValue"), color, sizeof(color));
    };
    checkPixels(fixture.render("uniform vec4 colorValue;", "return vec4(colorValue);", bindColor), {.2f,.4f,.6f,.8f});
    checkPixels(fixture.render("uniform vec4 colorValue;", "return vec4(vec3(colorValue), 1.0);", bindColor), {.2f,.4f,.6f,1});
    checkPixels(fixture.render("uniform vec4 colorValue;", "return vec4(vec2(vec3(colorValue)), 0.6, 0.8);", bindColor), {.2f,.4f,.6f,.8f});
    const float shortVectorData[3] = {.2f,.4f,.6f};
    checkPixels(fixture.render("uniform vec3 colorValue;", "return vec4(vec3(colorValue), 1.0);",
        [&](const auto& program) { program.setUniform(program.getUniformBinding("colorValue"), shortVectorData, sizeof(shortVectorData)); }), {.2f,.4f,.6f,1});
    checkPixels(fixture.render("", "return vec4(0.25);"), {.25f,.25f,.25f,.25f});
    const float transform[9] = {2,0,0, 0,3,0, 0,0,4};
    checkPixels(fixture.render("uniform mat3 transform;", "return vec4(transform * vec3(0.1, 0.2, 0.3), 1.0);",
        [&](const auto& program) { program.setUniform(program.getUniformBinding("transform"), transform, sizeof(transform)); }), {.2f,.6f,1.2f,1});
    auto invalid = fixture.pool.acquire("material Invalid { vertex { return vec4(0.0) } fragment { return vec4(vec3(1.0)) } }");
    CHECK(!invalid.isValid());
    CHECK(!fixture.pool.lastCompileErrors().empty());
    if (!fixture.pool.lastCompileErrors().empty())
        CHECK(fixture.pool.lastCompileErrors().front().find("requires at least 4") != std::string::npos);
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}

TEST_CASE(phoskia_complete_engine_catalog_installs_on_real_device_without_compiler) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend || !backend[0]) SKIP_TEST("Requires isolated D3D11/12 device");
    const auto jobs=ayt::render::detail::builtinShaderJobs();
    ayt::shader::ShaderBuildManifest manifest;
    manifest.target={AY_SHADER_SHADERC_HINT,"windows","s_5_0",{AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT}};
    manifest.variantBudget=128;manifest.jobs=jobs;
    ayt::shader::ShaderBuildSession session;ayt::shader::ShaderBuildReport report;
    session.build(manifest,report);CHECK(report.success);if(!report.success)return;
    PhoskiaGpuFixture fixture;const bool initialized=fixture.initialize(backend);CHECK(initialized);if(!initialized)return;
    fixture.pool.setShadercExecutable("missing-engine-compiler.exe");
    const auto packageRoot=std::filesystem::temp_directory_path()/("engine_production_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path root; ~Cleanup() {std::error_code ec;std::filesystem::remove_all(root,ec);} } cleanup{packageRoot};
    struct Environment {
        std::string name, previous; bool existed;
        Environment(const char* key,const std::string& value):name(key),existed(std::getenv(key)!=nullptr) {
            if(existed) previous=std::getenv(key);_putenv_s(key,value.c_str());
        }
        ~Environment() {_putenv_s(name.c_str(),existed?previous.c_str():"");}
    } rootOverride("AY_SHADER_PACKAGE_ROOT",packageRoot.string()), compilationOverride("AY_SHADER_RUNTIME_COMPILE","");
    std::vector<std::string> errors;
    CHECK(ayt::shader::writeShaderBuildPackage(report,(packageRoot/"windows-s_5_0"/"engine").string(),errors));
    const auto policy=(packageRoot/"production.policy").string();
    CHECK(ayt::io::File::writeAllText(policy,"AYPhoskiaProduction/1\n"));
    const bool configured=ayt::render::detail::configureShaderPool(fixture.pool);
    CHECK(configured);if(!configured)return;
    for(const auto& job:jobs) {
        const auto program=job.vertexSc.empty()?fixture.pool.acquire(job.sourceText,"device-catalog"):
            fixture.pool.acquireFromBgfxSc(job.vertexSc,job.fragmentSc,job.varyingSc,"device-catalog");
        CHECK_MSG(program.isValid(),job.name.c_str());
    }
    CHECK(fixture.pool.cacheStats().binaryMisses==0);
    CHECK(fixture.pool.cacheStats().sourceMisses==0);
    CHECK(!fixture.pool.acquire("material Missing {vertex {return vec4(0.0)} fragment {return vec4(1.0)}}").isValid());
    CHECK(ayt::io::File::writeAllText(policy,"unknown-policy\n"));
    CHECK(!ayt::render::detail::configureShaderPool(fixture.pool));
    CHECK(ayt::io::File::writeAllText(policy,"AYPhoskiaProduction/1\n"));
    CHECK(ayt::io::File::writeAllText((packageRoot/"authoring.policy").string(),"AYPhoskiaAuthoring/1\n"));
    // Relocated authoring products must not depend on build-machine hints.
    const auto authoringTools=packageRoot/"authoring";
    std::filesystem::create_directories(authoringTools);
    CHECK(!ayt::render::detail::configureShaderPool(fixture.pool));
    std::filesystem::copy_file(AY_SHADER_SHADERC_HINT,authoringTools/"shaderc.exe");
    std::filesystem::copy(AY_SHADER_BGFX_COMMON_HINT,authoringTools/"include-common",std::filesystem::copy_options::recursive);
    std::filesystem::copy(AY_SHADER_BGFX_SRC_HINT,authoringTools/"include-src",std::filesystem::copy_options::recursive);
    struct DefaultCompilerRestore { ~DefaultCompilerRestore() {ayt::shader::AYShadercDriver::setDefaultExecutable(AY_SHADER_SHADERC_HINT);} } defaultCompilerRestore;
    CHECK(ayt::render::detail::configureShaderPool(fixture.pool));
    const auto edited=fixture.pool.acquire("material Edited {vertex {in p : position; return vec4(p,1.0)} fragment {return vec4(0.2,0.4,0.6,1.0)}}");
    CHECK(edited.isValid());
    const auto editedPixels=fixture.renderResource(edited);
    CHECK(editedPixels.size()==PhoskiaGpuFixture::size*PhoskiaGpuFixture::size);
    for(const auto& pixel:editedPixels) for(size_t channel=0;channel<4;++channel)
        CHECK(std::abs(pixel[channel]-Pixel{0.2f,0.4f,0.6f,1.0f}[channel])<1e-4f);
#else
    SKIP_TEST("Windows real-device fixture");
#endif
}
TEST_SUITE_END
