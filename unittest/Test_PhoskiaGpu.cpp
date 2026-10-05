#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "AYTest.h"
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
#include "detail/BgfxMatrix.h"

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
