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
#include <AYRenderer/EmissivePointCloud.h>
#include <AYMath/MathUtils.h>
#include "detail/BGFXAdapter.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/GpuResources.h"
#include "AYRenderer/SsrShaderSources.h"
#include "AYRenderer/VolumetricShaderSources.h"
#include "AYRenderer/PerformanceShaderSources.h"
#include "AYRenderer/DeferredDecalShaderSources.h"
#include "detail/DeferredDecalPass.h"
#include "detail/PerformanceLightPass.h"
#include "detail/VolumetricPass.h"
#include "detail/FroxelHistorySource.h"
#include "detail/SsrPass.h"
#include "detail/LightingPass.h"
#include "detail/RenderViewOrder.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/PassExecContext.h"
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

    std::vector<Pixel> readImage(bgfx::TextureHandle image,uint8_t blitView=2) {
        bgfx::blit(blitView,readback,0,0,image);
        std::vector<Pixel> pixels(size*size);
        const auto ready=bgfx::readTexture(readback,pixels.data());
        for(int frame=0;frame<120 && adapter.gpuFrameCounter()<ready;++frame) adapter.endFrame();
        CHECK(adapter.gpuFrameCounter()>=ready);
        return pixels;
    }

    std::vector<Pixel> render(const std::string& declarations, const std::string& body,
        const std::function<void(const ayt::shader::ShaderResource&)>& bind = {},uint8_t viewId=0) {
        const auto source = "material Probe { " + declarations +
            " vertex { in position : position; return vec4(position, 1.0) } fragment { " + body + " } }";
        return renderSource(source, bind,viewId);
    }

    std::vector<Pixel> renderSource(const std::string& source,
        const std::function<void(const ayt::shader::ShaderResource&)>& bind = {},uint8_t viewId=0) {
        auto program = pool.acquire(source);
        return renderResource(program, bind,viewId);
    }

    std::vector<Pixel> renderResource(const ayt::shader::ShaderResource& program,
        const std::function<void(const ayt::shader::ShaderResource&)>& bind = {},uint8_t viewId=0) {
        CHECK(program.isValid());
        if (!program.isValid()) {
            for (const auto& error : pool.lastCompileErrors()) std::cerr << error << '\n';
            return {};
        }
        configureFullscreenPassView(adapter, viewId, target, 0, 0, size, size);
        geometry.bind(adapter);
        if (bind) bind(program);
        adapter.setStateDepthTestAlways();
        program.submit({viewId, 0});
        bgfx::blit(viewId+1, readback, 0, 0, targetTexture);
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
    for(int mode=0;mode<8;++mode) {
        const float density=mode==0?0.f:.1f;
        // Local medium fades over its outer quarter; its integral is 0.875.
        const float distance=mode==2?.25f*.875f:mode==3?0.f:1.f;
        const float t=std::exp(-density*distance);
        const float phaseRatio=mode==5?(.75f/std::pow(2.25f,1.5f)):1.f;
        const float expected=mode==6?2*t:mode==7?.8f*(1-t):2*t+(mode==4?0.f:.8f*(1-t)*phaseRatio);
        auto pixels=f.renderResource(program,[&](const auto& p){
            auto tex=[&](const char* n,bgfx::TextureHandle h){auto b=p.getTextureBinding(n);CHECK(b!=ayt::shader::InvalidBinding);p.setTexture(p.getTextureStage(b),b,toShaderTexture(h));};
            tex("sceneColor",scene);tex("worldPosition",world);tex("geometryCoverage",sky);tex("shadowAtlas",mode==4?dark:lit);tex("patternGobo",lit);
            auto identity=ayt::math::Float4x4::identity();
            for(const char* n:{"fogInverseViewProjection","fogView","fogShadow0","fogShadow1","fogShadow2"})p.setUniform(p.getUniformBinding(n),&identity,64);
            auto u=[&](const char* n,Pixel v){auto b=p.getUniformBinding(n);CHECK(b!=ayt::shader::InvalidBinding);p.setUniform(b,v.data(),16);};
            u("fogCamera",{0,0,0,1});u("fogMedium",{density,.8f,mode==5?.5f:0.f,100});u("fogOptions",{32,0,mode==2||mode==3?1.f:0.f,mode==6?1.f:mode==7?2.f:0.f});
            u("fogCenter",{0,0,mode==3?3.f:0.f,0});u("fogExtent",{10,10,.25f,0});u("fogTint",{1,1,1,0});
            u("fogLightPosition",{0,0,0,0});u("fogLightDirection",{0,0,1,0});u("fogLightColor",{12.5663706f,12.5663706f,12.5663706f,0});u("fogSpot",{0,0,0,0});
            u("fogPatternSettings",{0,0,1,0});u("fogPatternOptics",{0,1,0,0});u("fogPatternRight",{1,0,0,0});u("fogPatternUp",{0,1,0,0});
            u("fogLightBoundsCenter",{0,0,0,0});u("fogLightBoundsExtent",{10000,10000,10000,0});
            for(const char* n:{"fogShadowRect0","fogShadowRect1","fogShadowRect2"})u(n,{0,0,1,1});
            u("fogShadowParams",{0,0,0,mode==4?1.f:0.f});
        });
        checkPixels(pixels,{expected,expected,expected,mode==7?0.f:1.f});
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_beam_geometry_matches_analytic_chords_core_and_penumbra) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    // Encode the known [0,30] ray segment as [0,1] for consistent D3D11/D3D12 readback.
    const std::string source=std::string(ayt::render::kPerformancePatternSource)+R"(
material BeamGeometryProbe {
 uniform vec4 rayOrigin
 uniform vec4 rayDirection
 uniform vec4 query
 uniform vec4 cone
 vertex {in p:position;return vec4(p,1.0)}
 fragment {
  let hit=performanceBeamInterval(rayOrigin.xyz,rayDirection.xyz,vec3(0.0),vec3(0.0,0.0,1.0),cone,20.0,vec2(0.0,30.0))
  let profile=performanceBeamProfile(query.xyz,vec3(0.0,0.0,1.0),cone,0.05)
  return vec4(hit.x/30.0,hit.y/30.0,profile,0.0)
 }
})";
    const auto program=f.pool.acquire(source,"beam_geometry_probe_v2");CHECK(program.isValid());
    struct Case {Pixel origin,direction,query,expected;};
    const float diagonal=1/std::sqrt(1.01f);
    const std::array cases{
        Case{{-2,0,10,0},{1,0,0,0},{.85f,0,10,0},{.9f,3.1f,.5f,0}},
        Case{{-2,2,10,0},{1,0,0,0},{1.2f,0,10,0},{0,0,0,0}},
        Case{{.2f,0,-1,0},{0,0,1,0},{.4f,0,10,0},{2,21,1,0}},
        Case{{0,0,10,0},{0,0,1,0},{.85f,0,10,0},{0,10,.5f,0}},
        Case{{.2f,0,3,0},{0,0,-1,0},{0,0,-1,0},{0,2,0,0}},
        Case{{.5f,0,0,0},{.1f*diagonal,0,diagonal,0},{1.1f,0,10,0},{0,0,0,0}},
        Case{{0,0,10,0},{.1f*diagonal,0,diagonal,0},{0,0,10,0},{0,10/diagonal,1,0}}
    };
    for(const auto& test:cases) {
        const auto pixels=f.renderResource(program,[&](const auto& shader){
            const auto uniform=[&](const char* name,Pixel value){shader.setUniform(shader.getUniformBinding(name),value.data(),16);};
            uniform("rayOrigin",test.origin);uniform("rayDirection",test.direction);uniform("query",test.query);
            uniform("cone",{std::cos(std::atan(.05f)),std::cos(std::atan(.1f)),.1f,.1f});
        });
        CHECK(pixels.size()==16);
        for(const auto& pixel:pixels)for(int channel=0;channel<4;++channel)
            CHECK(std::abs(pixel[channel]-test.expected[channel]/(channel<2?30.f:1.f))<(channel<2?.00004f:.001f));
    }
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_spot_volume_perspective_rays_match_independent_cpu_integration) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    const auto black=f.input({0,0,0,0}),lit=f.input({1,1,1,1});
    const ayt::math::FVector3 eye(0,8,-8);
    const auto view=ayt::math::lh::lookAt(eye,{0,3,0},{0,1,0});
    const auto projection=ayt::math::lh::perspective(55.f*3.141592653589793f/180,1,.1f,100);
    const auto inverse=(projection*view).inverse();float inverseUpload[16],viewUpload[16];toBgfxColumnMajor(inverse,inverseUpload);toBgfxColumnMajor(view,viewUpload);
    const auto identityCheck=inverse*(projection*view);float residual=0;
    for(int row=0;row<4;++row)for(int column=0;column<4;++column)residual=std::max(residual,std::abs(identityCheck(row,column)-(row==column?1.f:0.f)));
    CHECK(residual<.002f);
    const float aperture=.08f,radius=aperture+20*std::tan(std::acos(.9f));
    auto program=f.pool.acquire(ayt::render::kVolumetricPhoskiaSource,ayt::render::kVolumetricCacheKey);
    const auto pixels=f.renderResource(program,[&](const auto& p){
        const auto tex=[&](const char* n,bgfx::TextureHandle h){auto b=p.getTextureBinding(n);p.setTexture(p.getTextureStage(b),b,toShaderTexture(h));};
        const auto u=[&](const char* n,Pixel v){p.setUniform(p.getUniformBinding(n),v.data(),16);};
        tex("sceneColor",black);tex("worldPosition",black);tex("geometryCoverage",black);tex("shadowAtlas",lit);
        p.setUniform(p.getUniformBinding("fogInverseViewProjection"),inverseUpload,64);p.setUniform(p.getUniformBinding("fogView"),viewUpload,64);
        const auto identity=ayt::math::Float4x4::identity();for(const auto* n:{"fogShadow0","fogShadow1","fogShadow2"})p.setUniform(p.getUniformBinding(n),&identity,64);
        for(const auto* n:{"fogShadowRect0","fogShadowRect1","fogShadowRect2"})u(n,{0,0,1,1});
        u("fogShadowParams",{0,0,0,0});u("fogCamera",{0,8,-8,0});u("fogMedium",{.1f,.8f,0,30});u("fogOptions",{64,0,0,2});
        u("fogCenter",{0,0,0,0});u("fogExtent",{100,100,100,0});u("fogTint",{1,1,1,0});
        u("fogLightPosition",{0,7,0,2});u("fogLightDirection",{0,-1,0,20});u("fogLightColor",{500,0,0,0});u("fogSpot",{.97f,.9f,std::tan(std::acos(.9f)),aperture});
        u("fogPatternSettings",{0,0,1,0});u("fogPatternOptics",{0,1,0,0});u("fogPatternRight",{1,0,0,std::tan(std::acos(.97f))});u("fogPatternUp",{0,0,1,0});
        u("fogLightBoundsCenter",{0,-3,0,0});u("fogLightBoundsExtent",{radius,10,radius,0});
    });
    size_t nonzero=0;CHECK(pixels.size()==16);
    for(int y=0;y<4;++y)for(int x=0;x<4;++x) {
        const auto forward=(ayt::math::FVector3(0,3,0)-eye).normalize();
        const auto right=ayt::math::FVector3(0,1,0).cross(forward).normalize();
        const auto up=forward.cross(right);
        const float tanHalf=std::tan(55.f*3.141592653589793f/360);
        const auto direction=(forward+right*(((x+.5f)*.5f-1)*tanHalf)+up*((1-(y+.5f)*.5f)*tanHalf)).normalize();
        const double origin[3]={eye.x,eye.y,eye.z},d[3]={direction.x,direction.y,direction.z};
        // Independent dense reference: no cone roots or production AABB clipping.
        const double begin=0,end=30;
        double result=0;
        if(end>begin){constexpr int count=65536;const double ds=(end-begin)/count,stepT=std::exp(-.1*ds);double transmission=1;
            for(int i=0;i<count;++i){const double t=begin+(i+.5)*ds,px=origin[0]+d[0]*t,py=origin[1]+d[1]*t-7,pz=origin[2]+d[2]*t;
                const double distance=std::sqrt(px*px+py*py+pz*pz),cutoff=std::clamp(1-distance/20,0.,1.);
                const double core=aperture-py*std::tan(std::acos(.97f)),field=aperture-py*std::tan(std::acos(.9f));
                const double cone=py<=0?std::clamp((field-std::sqrt(px*px+pz*pz))/std::max(field-core,1e-6),0.,1.):0.;
                const double attenuation=cutoff*cutoff/std::max(distance*distance,.01)*cone*cone*(3-2*cone);
                result+=transmission*(1-stepT)*.8*500*attenuation/(4*3.141592653589793);transmission*=stepT;
            }
        }
        if(result>1e-5)++nonzero;
        if(pixels.size()==16){const auto actual=pixels[y*4+x][0];if(std::abs(actual-result)>=.0002)std::fprintf(stderr,"Spot ray %d,%d expected=%g actual=%g\n",x,y,result,actual);CHECK(std::abs(actual-result)<.0002);}
    }
    CHECK(nonzero>0);CHECK(nonzero<16);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_pass_rebinds_inputs_for_each_light_and_accumulates_both_colors) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    checkPixels(f.render("","return vec4(0.0)"),{0,0,0,0});
    const auto albedo=f.input({1,1,1,0}),normal=f.input({.5f,.5f,0,1}),world=f.input({0,0,0,1}),coverage=f.input({0,0,0,1});
    RenderResourceBlackboard blackboard;blackboard.beginFrame();blackboard.publishGBuffer(f.target,albedo,normal,world,coverage,coverage,4,4,1,true);
    ayt::render::PerformanceLight red,blue;red.id=1;red.light=ayt::render::Light::spot({0,0,-2},{0,0,1},10,10,.95f,.8f,{1,0,0});
    blue=red;blue.id=2;blue.light.color={0,0,1};const std::array lights{red,blue};
    ayt::render::PerformanceLighting packet;CHECK(packet.assign(lights,{}));
    ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;FrameContext frame;frame.cameraPosition={0,0,-1};
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.performanceLights=&packet;ctx.resourceBlackboard=&blackboard;
    configureFullscreenPassView(f.adapter,8,f.target,0,0,4,4);
    PerformanceLightPass pass;CHECK(pass.execute(ctx)==2);
    const float expected=float(.97/3.141592653589793*1.6);checkPixels(f.readImage(f.targetTexture,9),{expected,0,expected,0});
    pass.destroy(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_flux_zoom_scales_actual_surface_submission) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    checkPixels(f.render("","return vec4(0.0)"),{0,0,0,0});
    const auto albedo=f.input({1,1,1,0}),normal=f.input({.5f,.5f,0,1}),world=f.input({0,0,0,1}),coverage=f.input({0,0,0,1});
    RenderResourceBlackboard blackboard;blackboard.beginFrame();blackboard.publishGBuffer(f.target,albedo,normal,world,coverage,coverage,4,4,1,true);
    ayt::render::PerformanceLight red,blue;red.id=1;red.constantFlux=true;red.light=ayt::render::Light::spot({0,0,-2},{0,0,1},10,1000,.95f,.8f,{1,0,0});
    blue=red;blue.id=2;blue.light.color={0,0,1};const std::array lights{red,blue};
    ayt::render::PerformanceLighting packet;CHECK(packet.assign(lights,{}));
    ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;FrameContext frame;frame.cameraPosition={0,0,-1};
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.performanceLights=&packet;ctx.resourceBlackboard=&blackboard;
    configureFullscreenPassView(f.adapter,8,f.target,0,0,4,4);
    PerformanceLightPass pass;CHECK(pass.execute(ctx)==2);
    const double scale=(2.0-std::cos(2.0*3.141592653589793/180)-std::cos(3.0*3.141592653589793/180))/(2.0-double(.95f)-double(.8f));
    const float expected=float(.97/3.141592653589793*160*scale);checkPixels(f.readImage(f.targetTexture,9),{expected,0,expected,0});
    pass.destroy(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_point_pass_lights_surfaces_outside_spot_axis) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    checkPixels(f.render("","return vec4(0.0)"),{0,0,0,0});
    const auto albedo=f.input({1,1,1,0}),normal=f.input({.5f,.5f,0,1}),world=f.input({0,0,0,1}),coverage=f.input({0,0,0,1});
    RenderResourceBlackboard blackboard;blackboard.beginFrame();blackboard.publishGBuffer(f.target,albedo,normal,world,coverage,coverage,4,4,1,true);
    const std::array<ayt::render::EmissivePoint,2> points{{{9,{0,0,-2},{1,0,0},10,.1f},{42,{0,0,-2},{0,0,1},10,.1f}}};
    ayt::render::EmissivePointLightingSettings settings;settings.intensityScale=1;settings.range=10;
    ayt::render::EmissivePointLightingStats stats;std::vector<ayt::render::PerformanceLight> lights;
    CHECK(ayt::render::buildEmissivePointLights(points,settings,lights,stats));CHECK(stats.contributing==2);CHECK(stats.selectedLights==1);
    ayt::render::PerformanceLighting packet;CHECK(packet.assign(lights,{}));
    ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;FrameContext frame;frame.cameraPosition={0,0,-1};
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.performanceLights=&packet;ctx.resourceBlackboard=&blackboard;
    configureFullscreenPassView(f.adapter,8,f.target,0,0,4,4);
    PerformanceLightPass pass;CHECK(pass.execute(ctx)==1);
    const float expected=float(.97/3.141592653589793*1.6);checkPixels(f.readImage(f.targetTexture,9),{expected,0,expected,0});
    pass.destroy(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_volume_real_pass_extinguishes_once_and_adds_two_light_colors) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    checkPixels(f.render("","return vec4(2.0,2.0,2.0,1.0)"),{2,2,2,1});
    const auto albedo=f.input({1,1,1,0}),normal=f.input({.5f,.5f,0,1}),world=f.input({0,0,0,1}),sky=f.input({0,0,0,0});
    RenderResourceBlackboard blackboard;blackboard.beginFrame();blackboard.publishGBuffer(f.target,albedo,normal,world,sky,sky,4,4,1,true);
    blackboard.publishProduced(BlackboardResourceId::LightingColor,BlackboardResourceLifetime::External,f.target,f.targetTexture,4,4,1);
    FrameGraph graph(f.adapter);PostProcessGraphPlanInput input;input.width=input.height=4;input.finalLdr=true;input.sceneColor=f.target;input.gbuffer=f.target;input.volumetric=true;
    CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
    ayt::render::PerformanceLight red,blue;red.id=1;red.light=ayt::render::Light::spot({0,0,-20},{0,0,1},100,10000,.99f,.98f,{1,0,0});
    blue=red;blue.id=2;blue.light.color={0,0,1};const std::array lights{red,blue};ayt::render::PerformanceLighting packet;CHECK(packet.assign(lights,{}));
    ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;FrameContext frame;frame.volumetric.enabled=true;frame.volumetric.density=.1f;frame.volumetric.scattering=.8f;frame.volumetric.anisotropy=0;frame.volumetric.steps=32;
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.sceneFbo=f.target;ctx.performanceLights=&packet;ctx.resourceBlackboard=&blackboard;ctx.frameGraph=&graph;
    VolumetricPass pass;CHECK(pass.execute(ctx)==3);CHECK(packet.stats.volumeDraws==2);
    const auto output=f.adapter.getFboAttachment(graph.resolve(FgResourceId::VolumetricColor),0);
    const auto pixels=f.render("texture2d result","return sample(result,vec2(0.5,0.5))",[&](const auto& p){auto b=p.getTextureBinding("result");p.setTexture(p.getTextureStage(b),b,toShaderTexture(output));},34);
    double radiance=0,transmission=1;const double stepT=std::exp(-.1/32);
    for(int i=0;i<32;++i){const double z=(i+.5)/32.,distance=std::sqrt(.25*.25*2+(20+z)*(20+z));const double cutoff=1-distance/100;
        radiance+=transmission*(1-stepT)*.8*10000*cutoff*cutoff/(distance*distance)/(4*3.141592653589793);transmission*=stepT;}
    const double base=2*std::exp(-.1);
    CHECK(pixels.size()==16);for(const auto& p:pixels){CHECK(std::abs(p[0]-(base+radiance))<.003);CHECK(std::abs(p[1]-base)<.003);CHECK(std::abs(p[2]-(base+radiance))<.003);CHECK(p[3]==1);}
    pass.destroyResources(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_flux_zoom_scales_actual_volume_submission) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    checkPixels(f.render("","return vec4(2.0,2.0,2.0,1.0)"),{2,2,2,1});
    const auto albedo=f.input({1,1,1,0}),normal=f.input({.5f,.5f,0,1}),world=f.input({0,0,0,1}),sky=f.input({0,0,0,0});
    RenderResourceBlackboard blackboard;blackboard.beginFrame();blackboard.publishGBuffer(f.target,albedo,normal,world,sky,sky,4,4,1,true);
    blackboard.publishProduced(BlackboardResourceId::LightingColor,BlackboardResourceLifetime::External,f.target,f.targetTexture,4,4,1);
    FrameGraph graph(f.adapter);PostProcessGraphPlanInput input;input.width=input.height=4;input.finalLdr=true;input.sceneColor=f.target;input.gbuffer=f.target;input.volumetric=true;
    CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
    ayt::render::PerformanceLight red,blue;red.id=1;red.constantFlux=true;red.light=ayt::render::Light::spot({0,0,-20},{0,0,1},100,10000,.99f,.98f,{1,0,0});
    blue=red;blue.id=2;blue.light.color={0,0,1};const std::array lights{red,blue};ayt::render::PerformanceLighting packet;CHECK(packet.assign(lights,{}));
    ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;FrameContext frame;frame.volumetric.enabled=true;frame.volumetric.density=.1f;frame.volumetric.scattering=.8f;frame.volumetric.anisotropy=0;frame.volumetric.steps=32;
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.sceneFbo=f.target;ctx.performanceLights=&packet;ctx.resourceBlackboard=&blackboard;ctx.frameGraph=&graph;
    VolumetricPass pass;CHECK(pass.execute(ctx)==3);CHECK(packet.stats.volumeDraws==2);
    const auto output=f.adapter.getFboAttachment(graph.resolve(FgResourceId::VolumetricColor),0);
    const auto pixels=f.render("texture2d result","return sample(result,vec2(0.5,0.5))",[&](const auto& p){auto b=p.getTextureBinding("result");p.setTexture(p.getTextureStage(b),b,toShaderTexture(output));},34);
    double radiance=0,transmission=1;const double stepT=std::exp(-.1/32);
    for(int i=0;i<32;++i){const double z=(i+.5)/32.,distance=std::sqrt(.25*.25*2+(20+z)*(20+z));const double cutoff=1-distance/100;
        radiance+=transmission*(1-stepT)*.8*10000*cutoff*cutoff/(distance*distance)/(4*3.141592653589793);transmission*=stepT;}
    radiance*=(2.0-std::cos(2.0*3.141592653589793/180)-std::cos(3.0*3.141592653589793/180))/(2.0-double(.99f)-double(.98f));
    const double base=2*std::exp(-.1);
    CHECK(pixels.size()==16);for(const auto& p:pixels){CHECK(std::abs(p[0]-(base+radiance))<.003);CHECK(std::abs(p[1]-base)<.003);CHECK(std::abs(p[2]-(base+radiance))<.003);CHECK(p[3]==1);}
    pass.destroyResources(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_pass_does_not_leak_scissor_into_a_later_fullscreen_view) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    const auto albedo=f.input({1,1,1,0}),normal=f.input({.5f,.5f,0,1}),world=f.input({0,0,0,1}),coverage=f.input({0,0,0,1});
    RenderResourceBlackboard blackboard;blackboard.beginFrame();
    blackboard.publishGBuffer(f.target,albedo,normal,world,coverage,coverage,4,4,1,true);
    ayt::render::PerformanceLight light;light.light=ayt::render::Light::spot({0,0,-2},{0,0,1},10,10,.95f,.8f,{1,0,0});
    ayt::render::PerformanceLighting packet;CHECK(packet.assign({&light,1},{}));
    ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;
    std::unordered_map<uint64_t,GpuTexture> textures;std::unordered_map<uint64_t,GpuMaterial> materials;FrameContext frame;
    frame.cameraPosition={0,0,-1};
    PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,1,1,frame};
    ctx.performanceLights=&packet;ctx.resourceBlackboard=&blackboard;
    configureFullscreenPassView(f.adapter,8,f.target,0,0,4,4);
    PerformanceLightPass pass;CHECK(pass.execute(ctx)==1);
    // Same encoder frame: the one-pixel light draw runs before a four-pixel
    // fullscreen view. State preserved by ShaderResource::submit must not clip it.
    checkPixels(f.render("","return vec4(0.2,0.4,0.6,1.0)",{},8),{.2f,.4f,.6f,1});
    pass.destroy(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(performance_surface_gpu_matches_brdf_pattern_shadow_and_unlit_contract) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    const auto albedo=f.input({1,1,1,0}),normal=f.input({.5f,.5f,0,1}),world=f.input({0,0,0,1});
    const auto edge=f.input({.5f,0,0,1}),gobo=f.input({.5f,.5f,.5f,1});
    const auto unlit=f.input({0,0,0,3}),coverage=f.input({0,0,0,1}),dark=f.input({0,0,0,1});
    std::array<Pixel,16> shadowPixels{};
    for(size_t i=0;i<shadowPixels.size();++i)shadowPixels[i]={i%4>=2?1.f:0.f,0,0,1};
    const auto softShadow=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,
        BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,
        bgfx::copy(shadowPixels.data(),sizeof(shadowPixels)));
    CHECK(bgfx::isValid(softShadow));if(!bgfx::isValid(softShadow))return;f.textures.push_back(softShadow);
    auto program=f.pool.acquire(ayt::render::kPerformanceSurfaceSource,"performance_surface_gpu_v2_pattern_optics");
    CHECK(program.isValid());if(!program.isValid())return;
    for(int mode=0;mode<8;++mode) {
        const auto pixels=f.renderResource(program,[&](const auto& p){
            const auto tex=[&](const char* name,bgfx::TextureHandle h){const auto b=p.getTextureBinding(name);CHECK(b!=ayt::shader::InvalidBinding);p.setTexture(p.getTextureStage(b),b,toShaderTexture(h));};
            const auto u=[&](const char* name,Pixel v){const auto b=p.getUniformBinding(name);CHECK(b!=ayt::shader::InvalidBinding);p.setUniform(b,v.data(),16);};
            tex("surfaceAlbedo",albedo);tex("surfaceNormal",normal);tex("surfacePosition",mode==4?unlit:mode==5?edge:world);tex("surfaceCoverage",coverage);tex("shadowAtlas",mode==7?softShadow:dark);tex("patternGobo",gobo);
            u("performancePosition",{0,0,-2,10});u("performanceDirection",{0,0,1,0});u("performanceColor",{10,0,0,0});
            u("performanceCone",{.95f,.8f,.75f,0});u("performanceRight",{1,0,0,0});u("performanceUp",{0,1,0,0});
            u("performancePatternSettings",{mode==1?1.f:mode==2?3.f:mode==6?5.f:0.f,0,1,0});u("performanceEye",{0,0,-1,0});
            u("performancePatternOptics",{0,mode==5?.1f:1.f,0,0});
            const auto identity=ayt::math::Float4x4::identity();p.setUniform(p.getUniformBinding("performanceShadow"),&identity,64);
            u("performanceShadowRect",{0,0,1,1});u("performanceShadowSettings",{mode==3||mode==7?1.f:0.f,0,mode==7?1.f:0.f,mode==7?.25f:0.f});
        });
        const float red=mode>=2&&mode!=6&&mode!=7?0.f:float(.97/3.141592653589793*1.6)*(mode==1||mode==6||mode==7?.5f:1.f);
        checkPixels(pixels,{red,0,0,0});
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

TEST_CASE(ssr_real_pass_reflects_lit_receivers_without_environment_or_ambient) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    std::array<Pixel,16> positions{},normals{};
    for(int y=0;y<4;++y)for(int x=0;x<4;++x){
        const float px=(x+.5f)*.5f-1,py=1-(y+.5f)*.5f,z=x==0?2.f:2.55f;
        positions[y*4+x]={px,py,z,1};
        if(x==0){const float l=std::sqrt(px*px+py*py+z*z),nx=px/l-.8f,ny=py/l,nz=z/l-.6f,nl=std::sqrt(nx*nx+ny*ny+nz*nz);normals[y*4+x]={nx/nl*.5f+.5f,ny/nl*.5f+.5f,nz/nl*.5f+.5f,.05f};}
        else normals[y*4+x]={0,.5f,.5f,.05f};
    }
    auto grid=[&](const auto& data){auto t=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(data.data(),sizeof(data)));f.textures.push_back(t);return t;};
    const auto world=grid(positions),normal=grid(normals),surface=f.input({0,0,0,1}),albedo=f.input({1,1,1,1});
    FrameGraph graph(f.adapter);PostProcessGraphPlanInput in;in.width=in.height=4;in.finalLdr=true;in.sceneColor=f.target;in.gbuffer=f.target;in.ssr=true;CHECK(buildPostProcessGraphPlan(graph,in).compileSucceeded);
    RenderResourceBlackboard bb;ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;std::unordered_map<uint64_t,GpuMaterial> materials;
    FrameContext frame;frame.ssr.enabled=true;frame.ssr.maxDistance=3;frame.ssr.thickness=.02f;frame.cameraPosition={0,0,0};
    LightingPass lighting;SsrPass pass;PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.sceneFbo=f.target;ctx.resourceBlackboard=&bb;ctx.frameGraph=&graph;ctx.lightingPass=&lighting;
    CHECK(!lighting.iblActiveThisFrame());
    double previous=0;
    for(float radiance:{1.f,2.f}){
        f.render("","return vec4("+std::to_string(radiance)+",0.0,0.0,1.0)");
        bb.beginFrame();bb.publishGBuffer(f.target,albedo,normal,world,surface,surface,4,4,1,true);bb.publishProduced(BlackboardResourceId::LightingColor,BlackboardResourceLifetime::External,f.target,f.targetTexture,4,4,1);
        CHECK(pass.execute(ctx)==1);CHECK(bb.producedThisFrame(BlackboardResourceId::SsrColor));
        const auto output=f.adapter.getFboAttachment(graph.resolve(FgResourceId::SsrColor),0);
        const auto pixels=f.render("texture2d ssrResult","return sample(ssrResult,vec2(0.125,0.375))",[&](const auto& p){const auto b=p.getTextureBinding("ssrResult");p.setTexture(p.getTextureStage(b),b,toShaderTexture(output));},34);
        CHECK(pixels.size()==16);if(pixels.empty())break;
        std::fprintf(stderr,"[SSR no IBL] input=%.1f reflected=%.4f\n",radiance,pixels[0][0]);
        CHECK(pixels[0][0]>radiance*1.05f);CHECK(pixels[0][0]<radiance*2);CHECK(pixels[0][1]==0);CHECK(pixels[0][3]==1);
        if(previous>0)CHECK(std::abs(pixels[0][0]-previous*2)<.005);previous=pixels[0][0];
    }
    frame.ssr.enabled=false;CHECK(pass.execute(ctx)==0);pass.destroyResources(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(indirect_real_pass_transfers_color_once_and_respects_material_and_zero_paths) {
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!backend[0])SKIP_TEST("Isolated device required");
    PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
    std::array<Pixel,16> positions{},normals{};
    for(int y=0;y<4;++y)for(int x=0;x<4;++x){positions[y*4+x]={(x+.5f)*.5f-1,1-(y+.5f)*.5f,x==0?2.f:2.55f,1};normals[y*4+x]=x==0?Pixel{.570710678f,.5f,.994974747f,1}:Pixel{0,.5f,.5f,1};}
    auto grid=[&](const auto& data){auto t=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(data.data(),sizeof(data)));f.textures.push_back(t);return t;};
    const auto world=grid(positions),normal=grid(normals),surface=f.input({0,0,0,1}),dielectric=f.input({.5f,.5f,.5f,0}),metal=f.input({.5f,.5f,.5f,1}),black=f.input({0,0,0,0}),uncovered=f.input({0,0,0,0});
    FrameGraph graph(f.adapter);PostProcessGraphPlanInput in;in.width=in.height=4;in.finalLdr=true;in.sceneColor=f.target;in.gbuffer=f.target;in.ssr=true;CHECK(buildPostProcessGraphPlan(graph,in).compileSucceeded);
    RenderResourceBlackboard bb;ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;std::unordered_map<uint64_t,GpuMaterial> materials;
    FrameContext frame;frame.ssr.enabled=false;frame.screenIndirect.enabled=true;frame.screenIndirect.strength=1;frame.screenIndirect.rays=1;frame.screenIndirect.steps=24;frame.screenIndirect.maxDistance=3;frame.screenIndirect.thickness=.02f;frame.cameraPosition={0,0,0};
    LightingPass lighting;SsrPass pass;PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.sceneFbo=f.target;ctx.resourceBlackboard=&bb;ctx.frameGraph=&graph;ctx.lightingPass=&lighting;
    double unit=0;
    for(int mode=0;mode<8;++mode){
        frame.screenIndirect.strength=mode==5?.5f:1.f;frame.screenIndirect.rays=mode==6?4:1;frame.screenIndirect.steps=mode==6?32:24;
        const float radiance=mode==1?2.f:mode==4?0.f:1.f;f.render("","return vec4("+std::to_string(radiance)+",0.0,0.0,1.0)");
        bb.beginFrame();bb.publishGBuffer(f.target,mode==2?metal:mode==3?black:dielectric,normal,world,mode==7?uncovered:surface,surface,4,4,1,true);bb.publishProduced(BlackboardResourceId::LightingColor,BlackboardResourceLifetime::External,f.target,f.targetTexture,4,4,1);
        CHECK(pass.execute(ctx)==1);const auto output=f.adapter.getFboAttachment(graph.resolve(FgResourceId::SsrColor),0);
        const auto pixels=f.render("texture2d indirectResult","return sample(indirectResult,vec2(0.125,0.375))",[&](const auto& p){const auto b=p.getTextureBinding("indirectResult");p.setTexture(p.getTextureStage(b),b,toShaderTexture(output));},34);
        CHECK(pixels.size()==16);if(pixels.empty())break;const auto value=pixels[0];std::fprintf(stderr,"[Indirect bounce] mode=%d input=%.1f output=%.4f\n",mode,radiance,value[0]);
        CHECK(value[1]==0);CHECK(value[2]==0);CHECK(value[3]==1);
        if(mode==0){CHECK(value[0]>1.05f);CHECK(value[0]<1.48f);unit=value[0];}
        else if(mode==1)CHECK(std::abs(value[0]-unit*2)<.005f);
        else if(mode==5)CHECK(std::abs(value[0]-(1+(unit-1)*.5))<.003f);
        else if(mode==6){CHECK(value[0]>=1);CHECK(value[0]<=1.48f);}
        else CHECK(std::abs(value[0]-radiance)<.001f);
    }
    frame.screenIndirect.strength=0;CHECK(pass.execute(ctx)==0);frame.screenIndirect.enabled=false;CHECK(pass.execute(ctx)==0);pass.destroyResources(f.adapter);
#else
    SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(reduced_volume_preserves_full_scene_edges_and_extinguishes_only_once) {
#ifdef _WIN32
 const auto* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Isolated device required");
 PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
 std::array<Pixel,16> colors{},positions{};for(int y=0;y<4;++y)for(int x=0;x<4;++x){colors[y*4+x]=x%2?Pixel{0,1,.25f,.75f}:Pixel{1,0,.25f,.75f};positions[y*4+x]={0,0,x<2?1.f:10.f,1};}
 const auto grid=[&](const auto& data){const auto t=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(data.data(),sizeof(data)));f.textures.push_back(t);return t;};
 const auto checker=grid(colors),world=grid(positions),normal=f.input({.5f,.5f,1,1}),coverage=f.input({0,0,0,1});
 const auto copy=[&](bgfx::TextureHandle image,uint8_t view){return f.renderSource("material VolumeProbe {texture2d probeImage vertex {in pos:position;out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5);return vec4(pos.xy,0.0,1.0)} fragment {in vUv:texcoord;return sample(probeImage,vec2(vUv.x,1.0-vUv.y))}}",[&](const auto& p){const auto b=p.getTextureBinding("probeImage");p.setTexture(p.getTextureStage(b),b,toShaderTexture(image));},view);};
 FrameGraph graph(f.adapter);RenderResourceBlackboard bb;ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;std::unordered_map<uint64_t,GpuMaterial> materials;
 ayt::render::PerformanceLight lamp;lamp.id=1;lamp.light=ayt::render::Light::spot({0,0,-2},{0,0,1},20,0,.9f,.8f,{1,0,0});ayt::render::PerformanceLighting packet;CHECK(packet.assign({&lamp,1},{}));
 FrameContext frame;frame.volumetric.enabled=true;frame.volumetric.density=.2f;frame.volumetric.scattering=.8f;frame.volumetric.steps=16;
 PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.sceneFbo=f.target;ctx.resourceBlackboard=&bb;ctx.frameGraph=&graph;ctx.performanceLights=&packet;
 VolumetricPass pass;
 for(bool localBox:{false,true})for(float noise:{0.f,.4f}){
  frame.volumetric.local=localBox;frame.volumetric.center={0,0,3};frame.volumetric.halfExtent={2,2,3};
  frame.volumetric.noiseStrength=noise;frame.volumetric.noiseTime=3;std::vector<Pixel> reference;
  for(uint8_t factor:{1,2,4}){
   frame.volumetric.downsample=factor;copy(checker,0);
   bb.beginFrame();bb.publishGBuffer(f.target,checker,normal,world,coverage,coverage,4,4,1,true);bb.publishProduced(BlackboardResourceId::LightingColor,BlackboardResourceLifetime::External,f.target,f.targetTexture,4,4,1);
   PostProcessGraphPlanInput in;in.width=in.height=4;in.finalLdr=true;in.sceneColor=f.target;in.gbuffer=f.target;in.volumetric=true;in.volumetricDownsample=factor;CHECK(buildPostProcessGraphPlan(graph,in).compileSucceeded);
   CHECK(pass.execute(ctx)==(factor==1?1:2));CHECK(packet.stats.volumeDraws==0);
   const auto output=f.adapter.getFboAttachment(graph.resolve(FgResourceId::VolumetricColor),0);const auto pixels=copy(output,244);CHECK(pixels.size()==16);if(pixels.size()!=16)break;
   if(factor==1)reference=pixels;
   for(size_t i=0;i<16;++i){for(int c=0;c<4;++c)CHECK(std::abs(pixels[i][c]-reference[i][c])<.001f);CHECK(std::abs(pixels[i][3]-.75f)<.001f);CHECK(pixels[i][int(i%4)%2?0:1]==0);}
   if(noise==0&&!localBox)for(int y=0;y<4;++y)for(int x=0;x<4;++x){const float px=(x+.5f)*.5f-1,py=1-(y+.5f)*.5f,z=x<2?1.f:10.f,T=std::exp(-.2f*std::sqrt(px*px+py*py+z*z));const auto i=y*4+x;CHECK(std::abs(pixels[i][x%2?1:0]-T)<.001f);}
  }
 }
 pass.destroyResources(f.adapter);
#else
 SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(froxel_history_reprojects_coefficients_and_rejects_flash_color_density_and_offscreen) {
#ifdef _WIN32
 const auto* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Isolated real-device acceptance required");
 PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
 std::string source=kFroxelHistorySource;const std::string at="let at=vec2(vUv.x,1.0-vUv.y)";const auto offset=source.find(at);CHECK(offset!=std::string::npos);source.replace(offset,at.size(),"let at=vec2(0.375)");
 const auto program=f.pool.acquire(source);CHECK(program.isValid());
 for(int mode=0;mode<8;++mode){
  std::array<Pixel,16> current{},old{};current.fill({2,0,0,.2f});current[5]={1,0,0,.2f};old.fill({1.4f,0,0,.2f});
  Pixel expected={mode==0||mode==7?1.2f:1.f,0,0,.2f};
  if(mode==2){current.fill({0,0,0,.2f});expected={0,0,0,.2f};}
  if(mode==3){current.fill({0,1,0,.2f});expected={0,1,0,.2f};}
  if(mode==4){for(auto& p:current)p[3]=.4f;expected[3]=.4f;}
  if(mode==5)old.fill({20,0,0,.2f});
  if(mode==7){old.fill({20,0,0,.2f});old[6]={1.4f,0,0,.2f};}
  const auto texture=[&](const auto& data){const auto value=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(data.data(),sizeof(data)));f.textures.push_back(value);return value;};
  const auto currentTexture=texture(current),oldTexture=texture(old);
  const auto pixels=f.renderResource(program,[&](const auto& p){
   const auto u=[&](const char* name,const void* data,uint32_t bytes){const auto b=p.getUniformBinding(name);CHECK(b!=ayt::shader::InvalidBinding);p.setUniform(b,data,bytes);};
   const auto v=[&](const char* name,Pixel data){u(name,data.data(),16);};
   float inverse[16];toBgfxColumnMajor(ayt::math::Float4x4::identity(),inverse);u("historyInverseViewProjection",inverse,64);
   auto previous=ayt::math::Float4x4::identity();if(mode==6)previous.row[0].w=10;if(mode==7)previous.row[0].w=.5f;toBgfxColumnMajor(previous,inverse);u("historyPreviousViewProjection",inverse,64);
   v("historyCamera",{0,0,0,1});v("historyPreviousCamera",{0,0,0,0});v("historyPreviousDirection",{0,0,1,0});v("historyPacking",{1,1,1,1});v("historyOptions",{.5f,mode==1?0.f:1.f,0,0});v("historyGrid",{.25f,.25f,0,0});
   const auto tex=[&](const char* name,bgfx::TextureHandle value){const auto b=p.getTextureBinding(name);CHECK(b!=ayt::shader::InvalidBinding);p.setTexture(p.getTextureStage(b),b,toShaderTexture(value));};tex("currentField",currentTexture);tex("previousField",oldTexture);
  });
  std::fprintf(stderr,"[Froxel history kernel] mode=%d\n",mode);checkPixels(pixels,expected);
 }
#else
 SKIP_TEST("Windows device fixture required");
#endif
}

TEST_CASE(froxel_coefficients_follow_beer_and_preserve_atlas_resize_and_two_light_energy) {
#ifdef _WIN32
 const auto* backend=std::getenv("AY_TAA_GPU_TEST");if(!backend||!*backend)SKIP_TEST("Isolated device required");
 PhoskiaGpuFixture f;CHECK(f.initialize(backend));if(!f.adapter.isInitialized())return;
 bgfx::setViewOrder(0,kRenderViewCapacity,kRenderViewRemap.data());
 std::array<Pixel,16> positions{};for(int y=0;y<4;++y)for(int x=0;x<4;++x)positions[y*4+x]={(x+.5f)*.5f-1,1-(y+.5f)*.5f,1,1};
 const auto world=bgfx::createTexture2D(4,4,false,1,bgfx::TextureFormat::RGBA32F,BGFX_SAMPLER_POINT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(positions.data(),sizeof(positions)));f.textures.push_back(world);
 const auto white=f.input({1,1,1,0}),normal=f.input({.5f,.5f,0,1}),surface=f.input({0,0,0,1});
 FrameGraph graph(f.adapter);RenderResourceBlackboard bb;ayt::render::RenderScene scene;std::unordered_map<uint64_t,GpuMesh> meshes;std::unordered_map<uint64_t,GpuTexture> textures;std::unordered_map<uint64_t,GpuMaterial> materials;
 FrameContext frame;frame.volumetric.enabled=true;frame.volumetric.froxel=true;frame.volumetric.froxelTileSize=4;frame.volumetric.froxelSlices=16;frame.volumetric.maxDistance=2;frame.volumetric.density=.2f;frame.volumetric.anisotropy=0;frame.volumetric.scattering=.8f;frame.lightDirection={0,0,-1};frame.lightColor={2,0,0};
 PassExecContext ctx{f.adapter,f.pool,scene,meshes,textures,materials,0,0,4,4,frame};ctx.sceneFbo=f.target;ctx.resourceBlackboard=&bb;ctx.frameGraph=&graph;
 const auto prepare=[&]{
  configureRenderViewOrder(f.adapter);f.render("","return vec4(1.0)",{},3);configureRenderViewOrder(f.adapter);
  bb.beginFrame();bb.publishGBuffer(f.target,white,normal,world,surface,surface,4,4,1,true);bb.publishProduced(BlackboardResourceId::LightingColor,BlackboardResourceLifetime::External,f.target,f.targetTexture,4,4,1);
  PostProcessGraphPlanInput in;in.width=in.height=4;in.finalLdr=true;in.sceneColor=f.target;in.gbuffer=f.target;in.volumetric=true;in.volumetricGrid=runtimeVolumetricGridLayout(frame.volumetric,4,4);CHECK(in.volumetricGrid.valid());CHECK(buildPostProcessGraphPlan(graph,in).compileSucceeded);
 };
 const auto probe=[&](bgfx::TextureHandle texture,float u,float v){configureRenderViewOrder(f.adapter);f.render("texture2d gridProbe","return sample(gridProbe,vec2("+std::to_string(u)+","+std::to_string(v)+"))",[&](const auto& p){const auto b=p.getTextureBinding("gridProbe");CHECK(b!=ayt::shader::InvalidBinding);p.setTexture(p.getTextureStage(b),b,toShaderTexture(texture));},244);
  // Read the completed probe in a separate submission; a remapped view must
  // never make this oracle observe the previous contents of the target.
  return f.readImage(f.targetTexture,200);};
 VolumetricPass pass;prepare();CHECK(pass.execute(ctx)==4);CHECK(pass.stats().froxel);CHECK(pass.stats().froxelCells==16);CHECK(pass.stats().integrationWidth==1);CHECK(pass.stats().depthSlices==16);
 auto pixels=probe(f.adapter.getFboAttachment(graph.resolve(FgResourceId::VolumetricColor),0),.5f,.5f);CHECK(pixels.size()==16);
 const float T=std::exp(-.2f),L=(1-T)*.8f*2/(4*3.141592653589793f);for(const auto& p:pixels){CHECK(std::abs(p[0]-(T+L))<.001f);CHECK(std::abs(p[1]-T)<.001f);CHECK(p[3]==1);}
 std::fprintf(stderr,"[Froxel Beer] red=%.6f green=%.6f expected=%.6f/%.6f cells=%u\n",pixels[0][0],pixels[0][1],T+L,T,pass.stats().froxelCells);
 auto atlas=probe(f.adapter.getFboAttachment(graph.resolve(FgResourceId::FroxelField),0),.0625f,.25f);if(!atlas.empty())std::fprintf(stderr,"[Froxel atlas] red=%.6f sigma=%.6f\n",atlas[0][0],atlas[0][3]);CHECK(!atlas.empty());if(!atlas.empty()){CHECK(std::abs(atlas[0][3]-.2f)<.00001f);CHECK(std::abs(atlas[0][0]-.8f*2/(4*3.141592653589793f))<.00001f);}
 ayt::render::PerformanceLight red,blue;red.id=1;red.light=ayt::render::Light::spot({0,0,-20},{0,0,1},100,10000,.99f,.98f,{1,0,0});blue=red;blue.id=2;blue.light.color={0,0,1};const std::array lights{red,blue};ayt::render::PerformanceLighting packet;CHECK(packet.assign(lights,{}));ctx.performanceLights=&packet;frame.volumetric.density=.1f;frame.volumetric.froxelSlices=64;
 prepare();CHECK(pass.execute(ctx)==5);CHECK(pass.stats().froxelCells==64);CHECK(packet.stats.volumeDraws==2);
 pixels=probe(f.adapter.getFboAttachment(graph.resolve(FgResourceId::VolumetricColor),0),.5f,.5f);CHECK(pixels.size()==16);double expected=0;for(int i=0;i<10000;++i){const double z=(i+.5)/10000.,d=20+z,cut=1-d/100;expected+=std::exp(-.1*z)*.1*.8*10000*cut*cut/(d*d)/(4*3.141592653589793)/10000;}
 if(!pixels.empty())std::fprintf(stderr,"[Froxel packet] red=%.6f green=%.6f expectedL=%.6f\n",pixels[0][0],pixels[0][1],expected);
 for(const auto& p:pixels){CHECK(std::abs(p[0]-(std::exp(-.1)+expected))<.003);CHECK(std::abs(p[2]-p[0])<.0001f);CHECK(std::abs(p[1]-std::exp(-.1))<.001);CHECK(p[3]==1);}
 atlas=probe(f.adapter.getFboAttachment(graph.resolve(FgResourceId::FroxelField),0),.0625f,.5625f);CHECK(!atlas.empty());if(!atlas.empty()){const double a=std::exp(std::log(3.)*32/64)-1,b=std::exp(std::log(3.)*33/64)-1,d=20+(a+b)*.5,cut=1-d/100,coefficient=.8*10000*cut*cut/(d*d)/(4*3.141592653589793);CHECK(std::abs(atlas[0][0]-coefficient)<.0001);CHECK(std::abs(atlas[0][2]-coefficient)<.0001);CHECK(std::abs(atlas[0][3]-.1f)<.00001f);}
 pass.destroyResources(f.adapter);
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
