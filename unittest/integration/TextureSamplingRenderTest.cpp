#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "AYRenderer.h"
#include "AYResource/ResourceBootstrap.h"
#include "AYResource/ResourceManager.h"
#include "AYResource/assetsImpl/Texture.h"
#include "detail/TextureImageLoader.h"
#include <bgfx/bgfx.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace ayt::render;
using namespace ayt::math;
using Clock = std::chrono::steady_clock;
constexpr uint16_t width = 640, height = 360;
constexpr int frameCount = 24;

void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
std::string text(const fs::path& path) {
    const auto value = path.generic_u8string(); return {value.begin(),value.end()};
}
const char* shaderSource = R"(
material SamplingProbe {
    texture2d albedoMap
    vertex {
        in position : position
        in uv : texcoord
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(position,1.0)
    }
    fragment { in uvOut : texcoord; return sample(albedoMap,uvOut); }
})";

struct Variant {
    const char* name;
    bool fullChain;
    ayt::resource::TextureFilter filter;
    std::vector<ayt::render::detail::DecodedImage> images;
    std::vector<double> submitMs,gpuMs;
    size_t encodedBytes = 0;
};

ayt::resource::Texture textureFor(const Variant& variant,bool stripes) {
    using namespace ayt::resource;
    Texture source; source._width = source._height = 512;
    std::vector<uint8_t> data; std::vector<uint32_t> offsets,sizes;
    for (uint32_t side = 512,level = 0; ; side /= 2,++level) {
        offsets.push_back(static_cast<uint32_t>(data.size())); sizes.push_back(side*side*4);
        for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x) {
            // Base is a known Nyquist checker. Every supplied coarser mip is
            // its independent exact area-average, rounded to nearest UNORM.
            const uint8_t color = stripes
                ? (level >= 5 ? 128 : ((x/(16u>>level))%2 ? 255 : 0))
                : (level == 0 ? ((x+y)%2 ? 255 : 0) : 128);
            data.insert(data.end(),{color,color,color,255});
        }
        if (!variant.fullChain || side == 1) break;
    }
    source.setImageData(std::move(data)); source.setMipmapLayout(std::move(offsets),std::move(sizes));
    TextureSampler sampler; sampler.minFilter = variant.filter;
    sampler.magFilter = TextureFilter::Bilinear; sampler.anisotropy = 16;
    source.setSampler(sampler); return source;
}

double percentile(std::vector<double> values,double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(),values.end());
    return values[std::min(values.size()-1,size_t(fraction*(values.size()-1)))];
}
} // namespace

int main(int argc,char** argv) {
    try {
        require(argc >= 3,"Usage: AYRenderer_TextureSamplingRenderTest <fresh-output> d3d11|d3d12 [checker|stripes]");
        const std::string backend = argv[2];
        const std::string pattern = argc > 3 ? argv[3] : "checker";
        require(pattern == "checker" || pattern == "stripes","Choose checker or stripes");
        const bool stripes = pattern == "stripes";
        require(backend == "d3d11" || backend == "d3d12","Choose an explicit D3D backend");
        const fs::path output = fs::absolute(fs::u8path(argv[1]));
        require(!fs::exists(output),"Use a fresh output directory"); fs::create_directories(output);
        HWND window = CreateWindowExW(0,L"STATIC",L"Texture sampling acceptance",WS_OVERLAPPED,
            0,0,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        require(window != nullptr,"Create hidden render window");
        struct Window { HWND handle; ~Window() { DestroyWindow(handle); } } windowOwner{window};
        require(ayt::resource::initializeLoaders(),"Register actual resource loaders");
        auto& cpu = ayt::resource::ResourceManager::instance();
        cpu.setAutoWatchLoadedResources(false);
        Renderer renderer;
        InitDesc init; init.windowHandle = window; init.width = width; init.height = height;
        init.vsync = false; init.msaa = 0;
        init.backend = backend == "d3d12" ? Backend::Direct3D12 : Backend::Direct3D11;
        require(renderer.initialize(init),"Initialize real renderer and production shaders");
        renderer.configurePipeline(RenderPipelineDesc::makeDefault());
        renderer.setShadowsEnabled(false); renderer.setTaaEnabled(false);
        renderer.setFxaaEnabled(false); renderer.setSmaaEnabled(false);
        renderer.setPostProcessGamma(1); renderer.setPostProcessExposure(1);
        renderer.setPostProcessTonemapMode(Renderer::TonemapMode::None);
        const float uTiles = stripes ? 4.f : 32.f;
        const float vertices[][8] = {
            {-8,0,0, 0,1,0, 0,0}, {8,0,0, 0,1,0, uTiles,0},
            {8,0,48, 0,1,0, uTiles,192}, {-8,0,48, 0,1,0, 0,192}};
        // Both windings make the fixture independent of raster front-face
        // conventions, while depth testing still accepts a single surface.
        const uint16_t indices[]{0,1,2,0,2,3,2,1,0,3,2,0};
        VertexLayoutDesc layout;
        layout.add({VertexAttribute::Position,3,VertexComponentType::Float,false});
        layout.add({VertexAttribute::Normal,3,VertexComponentType::Float,false});
        layout.add({VertexAttribute::TexCoord0,2,VertexComponentType::Float,false});
        auto mesh = renderer.createMesh(vertices,4,layout,indices,12);
        auto material = renderer.createMaterialFromPhoskia(shaderSource,"sampling-implicit-lod-acceptance");
        require(mesh.isValid() && material.isValid(),"Create actual textured perspective surface");
        renderer.setMaterialModel(material,MaterialModel::Unlit);
        RenderScene scene; scene.add(mesh,material,Float4x4::identity());
        RenderScene empty;
        auto frame = [&](int index,const fs::path& capture,bool geometry = true) {
            const float x = float(index)*.0075f;
            const FVector3 eye{x,1.4f,-1.5f}, target{x,0,18};
            const auto view = lh::lookAt(eye,target,{0,1,0});
            const auto projection = lh::perspective(radians(58.f),float(width)/height,.05f,100.f);
            renderer.setMainCamera(view,projection,eye);
            ClearDesc clear; clear.r = clear.g = clear.b = .02f;
            renderer.beginFrame(clear); renderer.render(geometry ? scene : empty);
            if (!capture.empty()) require(renderer.captureScreenshot(text(capture)),"Queue real framebuffer capture");
            renderer.endFrame();
        };
        std::vector<Variant> variants{
            {"base-bilinear",false,ayt::resource::TextureFilter::Trilinear},
            {"full-bilinear",true,ayt::resource::TextureFilter::Bilinear},
            {"full-trilinear",true,ayt::resource::TextureFilter::Trilinear},
            {"full-anisotropic",true,ayt::resource::TextureFilter::Anisotropic}};
        for (auto& variant : variants) {
            fs::create_directories(output/variant.name);
            auto source = textureFor(variant,stripes); variant.encodedBytes = source.getImageDataSize();
            std::vector<uint8_t> binary; require(source.saveToBinary(binary),"Encode authored texture including sampler");
            const auto file = output/(std::string(variant.name)+".txr");
            {std::ofstream stream(file,std::ios::binary); stream.write(reinterpret_cast<const char*>(binary.data()),std::streamsize(binary.size()));
             require(bool(stream),"Write real texture resource");}
            auto texture = renderer.loadTexture(text(file)); require(texture.isValid(),"Upload texture through resource bridge");
            renderer.setMaterialTexture(material,"albedoMap",texture);
            for (int i = 0; i < 12; ++i) frame(0,{});
            for (int i = 0; i < frameCount; ++i) {
                for (int warm = 0; warm < 3; ++warm) frame(i,{});
                const auto capture = output/variant.name/("frame-"+std::to_string(i)+".png");
                frame(i,capture); for (int drain = 0; drain < 5; ++drain) frame(i,{});
                auto image = ayt::render::detail::decodeImageFile(text(capture));
                require(image.isValid() && image.width == width && image.height == height,"Read actual frame pixels");
                variant.images.push_back(std::move(image));
            }
            // Timing excludes shader creation, upload, capture and file IO.
            for (int i = 0; i < 16; ++i) frame(0,{});
            for (int i = 0; i < 120; ++i) {
                const auto start = Clock::now(); frame(i%frameCount,{});
                variant.submitMs.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
                const auto* stats = bgfx::getStats();
                if (stats->gpuTimerFreq > 0 && stats->gpuTimeEnd > stats->gpuTimeBegin)
                    variant.gpuMs.push_back(double(stats->gpuTimeEnd-stats->gpuTimeBegin)*1000/double(stats->gpuTimerFreq));
            }
            renderer.destroyTexture(texture);
        }
        const auto emptyPath = output/"empty.png";
        // The native screenshot callback may read the previously presented
        // swapchain image (notably D3D12). Settle the empty state before the
        // request, just as each textured camera sample is settled above.
        for (int i = 0; i < 8; ++i) frame(0,{},false);
        frame(0,emptyPath,false); for (int i = 0; i < 6; ++i) frame(0,{},false);
        const auto background = ayt::render::detail::decodeImageFile(text(emptyPath)); require(background.isValid(),"Read empty geometry reference");
        const auto& full = variants[2];
        size_t covered = 0;
        std::vector<size_t> mask;
        // U-only stripes use the near interior where the U signal is resolved
        // by base-level bilinear sampling. V is constant, making this a direct
        // reference for anisotropic detail without a hidden V-aliasing term.
        for (uint32_t y = stripes ? 235 : 100; y < (stripes ? 320u : 235u); ++y)
        for (uint32_t x = 100; x < width-100; ++x) {
            const size_t pixel = (size_t(y)*width+x)*4;
            bool valid = true;
            for (const auto& image : full.images)
                valid &= std::abs(int(image.rgba8[pixel])-int(background.rgba8[pixel])) > 10;
            if (valid) {mask.push_back(pixel);++covered;}
        }
        require(covered > 2000,"Perspective test geometry covers a stable interior region");
        nlohmann::json report{{"backend",backend},{"pattern",pattern},{"width",width},{"height",height},{"frames",frameCount},
            {"stableInteriorPixels",covered},{"scope","Explicit fixture Host, Forward Unlit, AA disabled; no Editor or all-scene performance claim"},
            {"variants",nlohmann::json::array()}};
        const auto* caps=bgfx::getCaps();
        report["device"]={{"renderer",bgfx::getRendererName(bgfx::getRendererType())},
            {"vendorId",caps->vendorId},{"deviceId",caps->deviceId},{"maxTextureSize",caps->limits.maxTextureSize}};
        for (const auto& variant : variants) {
            double difference = 0,referenceError = 0;
            for (size_t i = 1; i < variant.images.size(); ++i)
                for (size_t pixel : mask)
                    difference += std::abs(int(variant.images[i].rgba8[pixel])-int(variant.images[i-1].rgba8[pixel]));
            difference /= double(mask.size()*(frameCount-1));
            for (size_t i = 0; i < variant.images.size(); ++i) for (size_t pixel : mask)
                referenceError += std::abs(int(variant.images[i].rgba8[pixel])-int(variants[0].images[i].rgba8[pixel]));
            referenceError /= double(mask.size()*frameCount);
            report["variants"].push_back({{"name",variant.name},{"encodedUploadBytes",variant.encodedBytes},
                {"meanTemporalByteDifference",difference},{"meanErrorToBaseBilinearReference",referenceError},
                {"submitMedianMs",percentile(variant.submitMs,.5)},
                {"submitP95Ms",percentile(variant.submitMs,.95)},
                {"gpuTimingSamples",variant.gpuMs.size()},
                {"gpuMedianMs",variant.gpuMs.empty()?nlohmann::json(nullptr):nlohmann::json(percentile(variant.gpuMs,.5))}});
        }
        report["passed"] = stripes
            ? report["variants"][3]["meanErrorToBaseBilinearReference"].get<double>()
                < report["variants"][2]["meanErrorToBaseBilinearReference"].get<double>()
            : report["variants"][2]["meanTemporalByteDifference"].get<double>()
                < report["variants"][0]["meanTemporalByteDifference"].get<double>();
        {std::ofstream stream(output/"acceptance.json"); stream << report.dump(2) << '\n';}
        require(report["passed"].get<bool>(),stripes
            ? "Anisotropic sampling preserves resolved U detail better than isotropic trilinear on the fixed fixture"
            : "Full-chain trilinear sampling reduces measured temporal variation on the fixed fixture");
        renderer.destroyMaterial(material); renderer.destroyMesh(mesh); renderer.shutdown();cpu.unloadAll();
        std::cout << report.dump(2) << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
