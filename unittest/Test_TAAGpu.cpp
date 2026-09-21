#include "AYTest.h"
#include "detail/TAAPass.h"
#include "detail/MotionVectorPass.h"
#include "detail/BgfxMatrix.h"
#include "AYShader/ShaderResourcePool.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <bx/uint32_t.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

using namespace ayt::render::detail;
constexpr uint16_t kSize = 32;
using Pixel = std::array<float, 4>;

// Isolated, opt-in real-GPU test. Run only AYRenderer_TAAGpu in a fresh
// process: the portable suite intentionally keeps bgfx Noop alive.
// AY_TAA_GPU_TEST=d3d11 (or d3d12). No visible window or editor input.
struct TaaGpuFixture {
    HWND window = nullptr;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::shader::ShaderResource program;
    FullscreenPassGeometry geometry;
    std::vector<bgfx::TextureHandle> textures;
    bgfx::FrameBufferHandle target = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle targetTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle readback = BGFX_INVALID_HANDLE;
    bool productionPrecision = false;

    ~TaaGpuFixture() {
        if (adapter.isInitialized()) {
            program.reset();
            pool.shutdown();
            geometry.destroy(adapter);
            if (bgfx::isValid(target)) bgfx::destroy(target);
            for (auto texture : textures) bgfx::destroy(texture);
            adapter.shutdown();
        }
        if (window) DestroyWindow(window);
    }

    bool initialize(const char* backend, bool halfPrecision = false) {
        productionPrecision = halfPrecision;
        window = CreateWindowExW(0, L"STATIC", L"TAA GPU regression",
            WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!window) return false;
        BGFXInitParams init;
        init.nativeWindowHandle = window;
        init.width = init.height = 64;
        init.vsync = false;
        init.msaa = 0;
        init.backend = std::string(backend) == "d3d12"
            ? ayt::render::Backend::Direct3D12
            : ayt::render::Backend::Direct3D11;
        if (!adapter.initialize(init)) return false;
        const uint64_t required = BGFX_CAPS_TEXTURE_READ_BACK | BGFX_CAPS_TEXTURE_BLIT;
        if ((bgfx::getCaps()->supported & required) != required) return false;
        pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
        pool.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT, AY_SHADER_BGFX_SRC_HINT});
        pool.resolvePlatformFromRenderer();
        program = pool.acquire(taaPhoskiaSourceForTests());
        if (!program.isValid()) {
            for (const auto& error : pool.lastCompileErrors()) std::cerr << error << '\n';
            return false;
        }
        if (!geometry.ensure(adapter)) return false;
        targetTexture = bgfx::createTexture2D(kSize, kSize, false, 1,
            halfPrecision ? bgfx::TextureFormat::RGBA16F : bgfx::TextureFormat::RGBA32F,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_POINT);
        if (!bgfx::isValid(targetTexture)) return false;
        textures.push_back(targetTexture);
        target = bgfx::createFrameBuffer(1, &targetTexture, false);
        readback = bgfx::createTexture2D(kSize, kSize, false, 1,
            halfPrecision ? bgfx::TextureFormat::RGBA16F : bgfx::TextureFormat::RGBA32F,
            BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
        if (!bgfx::isValid(readback)) return false;
        textures.push_back(readback);
        return bgfx::isValid(target);
    }

    bgfx::TextureHandle input(const std::vector<Pixel>& pixels, bool point, bool byteColor = false) {
        const auto flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | (point ? BGFX_SAMPLER_POINT : 0);
        auto format = bgfx::TextureFormat::RGBA32F;
        const bgfx::Memory* memory = nullptr;
        if (productionPrecision && byteColor) {
            std::vector<uint8_t> bytes(pixels.size() * 4);
            for (size_t i = 0; i < pixels.size(); ++i)
                for (size_t c = 0; c < 4; ++c)
                    bytes[i * 4 + c] = static_cast<uint8_t>(std::clamp(pixels[i][c], 0.0f, 1.0f) * 255.0f + 0.5f);
            memory = bgfx::copy(bytes.data(), static_cast<uint32_t>(bytes.size()));
            format = bgfx::TextureFormat::RGBA8;
        } else if (productionPrecision) {
            std::vector<uint16_t> halves(pixels.size() * 4);
            for (size_t i = 0; i < pixels.size(); ++i)
                for (size_t c = 0; c < 4; ++c) halves[i * 4 + c] = bx::halfFromFloat(pixels[i][c]);
            memory = bgfx::copy(halves.data(), static_cast<uint32_t>(halves.size() * 2));
            format = bgfx::TextureFormat::RGBA16F;
        } else {
            memory = bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size() * sizeof(Pixel)));
        }
        auto texture = bgfx::createTexture2D(kSize, kSize, false, 1, format, flags, memory);
        if (bgfx::isValid(texture)) textures.push_back(texture);
        return texture;
    }

    std::vector<Pixel> resolve(TaaJitter jitter, float previousDepth,
                              bool velocityValid, bool mixedSky, float bias,
                              float storedDepth = -2.0f, bool historyValid = true,
                              float velocityPixels = 0.0f) {
        std::vector<Pixel> current(kSize * kSize), history(current.size());
        std::vector<Pixel> world(current.size(), Pixel{0, 0, 0.2f, 1});
        std::vector<Pixel> surface(current.size(), Pixel{0, 0, 0, 1});
        std::vector<Pixel> velocity(current.size(), Pixel{velocityPixels / kSize, 0, previousDepth, velocityValid ? 1.0f : 0.0f});
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const float fixed = 0.2f + 0.3f * (x + 0.5f) / kSize
                                        + 0.2f * (y + 0.5f) / kSize;
                const float raw = fixed - (0.3f * jitter.x + 0.2f * jitter.y) / kSize;
                current[y * kSize + x] = {raw, raw, raw, 1};
                history[y * kSize + x] = {fixed + bias, fixed + bias, fixed + bias,
                    storedDepth == -2.0f ? previousDepth : storedDepth};
                if (mixedSky && (x & 1)) {
                    history[y * kSize + x] = {1, 0, 0, -1};
                }
            }
        }
        // A quarter-texel velocity exercises four-tap depth validation.
        if (mixedSky) for (auto& v : velocity) v[0] = 0.25f / kSize;
        return dispatch(jitter, current, history, world, surface, velocity, historyValid);
    }

    std::vector<Pixel> dispatch(TaaJitter jitter,
        const std::vector<Pixel>& current, const std::vector<Pixel>& history,
        const std::vector<Pixel>& world, const std::vector<Pixel>& surface,
        const std::vector<Pixel>& velocity, bool historyValid, float debugMode = 0) {
        std::vector<Pixel> sceneDepth(current.size());
        for (size_t i = 0; i < sceneDepth.size(); ++i)
            sceneDepth[i] = {surface[i][3] > .5f ? world[i][2] : 1.0f, 0, 0, 0};
        const std::array<const std::vector<Pixel>*, 6> inputs = {
            &current, &history, &world, &surface, &velocity, &sceneDepth};
        const auto firstInput = textures.size();
        std::array<bgfx::TextureHandle,6> bindings;
        for (uint8_t i = 0; i < inputs.size(); ++i) {
            bindings[i] = input(*inputs[i], i >= 1, i == 0 || i == 3);
            CHECK(bgfx::isValid(bindings[i]));
        }
        submitResolve(jitter,bindings,historyValid,debugMode,target);
        auto result = readTarget();
        for (auto i = firstInput; i < textures.size(); ++i) bgfx::destroy(textures[i]);
        textures.resize(firstInput);
        return result;
    }

    void submitResolve(TaaJitter jitter, const std::array<bgfx::TextureHandle,6>& bindings,
                       bool historyValid, float debugMode, bgfx::FrameBufferHandle output) {
        const std::array<const char*, 6> names = {
            "currentColor", "historyColor", "worldPosition", "geometryData", "motionVectors", "sceneDepth"};
        configureFullscreenPassView(adapter, 0, output, 0, 0, kSize, kSize);
        geometry.bind(adapter);
        for (uint8_t i=0;i<bindings.size();++i)
            program.setTexture(i, program.getTextureBinding(names[i]), toShaderTexture(bindings[i]));
        const float metrics[4] = {1.0f / kSize, 1.0f / kSize, kSize, kSize};
        const float params[4] = {0.92f, 0.65f, 0.025f, historyValid ? 1.0f : 0.0f};
        const float offset[4] = {jitter.x / kSize, jitter.y / kSize, 1, 1};
        const float depth[4] = {TAAPass::kHistoryDepthTolerance, debugMode, 0, 0};
        const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        program.setUniform(program.getUniformBinding("taaMetrics"), metrics, sizeof(metrics));
        program.setUniform(program.getUniformBinding("taaParams"), params, sizeof(params));
        program.setUniform(program.getUniformBinding("taaJitter"), offset, sizeof(offset));
        program.setUniform(program.getUniformBinding("taaDepthParams"), depth, sizeof(depth));
        program.setUniform(program.getUniformBinding("currentViewProjection"), identity, sizeof(identity));
        adapter.setStateDepthTestAlways();
        program.submit({0, 0});
    }

    std::vector<Pixel> readTarget() {
        bgfx::blit(1, readback, 0, 0, targetTexture);
        std::vector<Pixel> result(kSize*kSize);
        std::vector<uint16_t> halves(result.size() * 4);
        const auto ready = bgfx::readTexture(readback,
            productionPrecision ? static_cast<void*>(halves.data()) : static_cast<void*>(result.data()));
        for (int i = 0; i < 120 && adapter.gpuFrameCounter() < ready; ++i) adapter.endFrame();
        CHECK(adapter.gpuFrameCounter() >= ready);
        if (productionPrecision) {
            for (size_t i = 0; i < result.size(); ++i)
                for (size_t c = 0; c < 4; ++c) result[i][c] = bx::halfToFloat(halves[i * 4 + c]);
        }
        return result;
    }
};

} // namespace
#endif

TEST_SUITE(AYRenderer_TAAGpu)

TEST_CASE(taa_production_shader_fixed_grid_depth_and_missing_velocity)
{
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend) {
        std::cout << "[SKIP] Set AY_TAA_GPU_TEST=d3d11/d3d12 and run isolated AYRenderer_TAAGpu.\n";
        return;
    }
    TaaGpuFixture fixture;
    const bool ready = fixture.initialize(backend);
    CHECK(ready);
    if (!ready) return;
    constexpr int x = 16, y = 16;
    constexpr float expected = 0.2f + 0.3f * (x + 0.5f) / kSize + 0.2f * (y + 0.5f) / kSize;
    for (uint32_t phase = 0; phase < 8; ++phase) {
        const auto jitter = taaHaltonJitter(phase);
        // History depth 0.8, current depth 0.2: valid same-surface previous
        // depth must win over the incorrect previous-camera/current-world test.
        const auto image = fixture.resolve(jitter, 0.8f, true, false, 0.002f);
        const auto p = image[y * kSize + x];
        CHECK(std::abs(p[0] - (expected + 0.002f * 0.92f)) < 0.0001f);
        CHECK(std::abs(p[1] - p[0]) < 0.0001f);
        CHECK(std::abs(p[3] - 0.2f) < 0.0001f);
        const auto missing = fixture.resolve(jitter, 0.8f, false, false, 0.002f);
        CHECK(std::abs(missing[y * kSize + x][0] - expected) < 0.0001f);
    }
    const auto edge = fixture.resolve({0.25f, -0.25f}, 0.8f, true, true, 0.002f);
    CHECK(edge[y * kSize + x][0] > expected + 0.001f);
    CHECK(std::abs(edge[y * kSize + x][0] - edge[y * kSize + x][1]) < 0.0001f);
    const auto occluded = fixture.resolve({0.25f, -0.25f}, 0.8f, true, false, 0.002f, 0.3f);
    CHECK(std::abs(occluded[y * kSize + x][0] - expected) < 0.0001f);
    const auto reset = fixture.resolve({0.25f, -0.25f}, 0.8f, true, false, 0.002f, -2.0f, false);
    CHECK(std::abs(reset[y * kSize + x][0] - expected) < 0.0001f);
    const auto outside = fixture.resolve({0.25f, -0.25f}, 0.8f, true, false, 0.002f, -2.0f, true, 64.0f);
    CHECK(std::abs(outside[y * kSize + x][0] - expected) < 0.0001f);
#else
    std::cout << "[SKIP] Windows D3D GPU regression.\n";
#endif
}

TEST_SUITE_END

TEST_SUITE(AYRenderer_MotionVectorGpu)

TEST_CASE(motion_rasterized_geometry_removes_jitter_without_removing_real_motion)
{
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend) { std::cout << "[SKIP] Isolated GPU suite requires AY_TAA_GPU_TEST.\n"; return; }
    TaaGpuFixture fixture;
    const bool ready = fixture.initialize(backend);
    CHECK(ready);
    if (!ready) return;
    struct Vertex { float p[3], uv[2], indices[4], weights[4]; };
    const Vertex vertices[] = {
        {{-1,-1,1},{0,1},{0,0,0,0},{1,0,0,0}},
        {{ 3,-1,1},{2,1},{0,0,0,0},{1,0,0,0}},
        {{-1, 3,1},{0,-1},{0,0,0,0},{1,0,0,0}}
    };
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position,3,bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0,2,bgfx::AttribType::Float)
        .add(bgfx::Attrib::Indices,4,bgfx::AttribType::Float)
        .add(bgfx::Attrib::Weight,4,bgfx::AttribType::Float).end();
    const auto vb = bgfx::createVertexBuffer(bgfx::copy(vertices,sizeof(vertices)),layout);
    CHECK(bgfx::isValid(vb));
    if (!bgfx::isValid(vb)) return;
    const auto white = fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{1,1,1,1}),true);
    const auto identity = ayt::math::Float4x4::identity();
    float identityData[16];
    toBgfxColumnMajor(identity,identityData);
    std::array<float,128*16> bones{};
    for (size_t i=0;i<128;++i) std::copy(identityData,identityData+16,bones.data()+16*i);
    for (int cutout=0;cutout<2;++cutout) {
        auto motion = fixture.pool.acquire(cutout ? motionVectorCutoutPhoskiaSourceForTests()
                                                 : motionVectorPhoskiaSourceForTests());
        CHECK(motion.isValid());
        if (!motion.isValid()) continue;
        float worstStatic=0, worstMoving=0;
        // Both projection types and rigid/skinned geometry, all eight phases.
        for (int perspective=0;perspective<2;++perspective)
        for (int skinned=0;skinned<2;++skinned)
        for (int sloped=0;sloped<2;++sloped)
        for (int phase=0;phase<8;++phase)
        for (int moving=0;moving<2;++moving) {
            auto base=identity;
            base(2,2)=.5f;
            base(2,0)=sloped ? .16f : 0;
            if (perspective) { base(3,2)=1; base(3,3)=0; }
            const auto jitter=taaHaltonJitter(phase);
            const auto previousJitter=taaHaltonJitter((phase+7)%8);
            const auto projection=taaApplyProjectionJitter(base,jitter,kSize,kSize);
            const auto previousProjection=taaApplyProjectionJitter(base,previousJitter,kSize,kSize);
            float previousData[16]; toBgfxColumnMajor(previousProjection,previousData);
            auto world=identity;
            world(0,3)=moving && !skinned ? 4.0f/kSize : 0;
            auto currentBones=bones;
            // A 2-pixel rigid translation or a 2-pixel bone translation.
            currentBones[12]=moving && skinned ? 4.0f/kSize : 0;
            fixture.adapter.setViewFrameBuffer(0,fixture.target);
            fixture.adapter.setViewRect(0,0,0,kSize,kSize);
            fixture.adapter.setViewTransform(0,identity,projection);
            fixture.adapter.setViewClearRaw(0,BGFX_CLEAR_COLOR,0);
            fixture.adapter.setTransform(world);
            bgfx::setVertexBuffer(0,vb);
            auto uniform=[&](const char* name,const void* data,size_t size) {
                motion.setUniform(motion.getUniformBinding(name),data,size);
            };
            uniform("previousWorld",identityData,sizeof(identityData));
            uniform("previousViewProjection",previousData,sizeof(previousData));
            uniform("bones",currentBones.data(),sizeof(currentBones));
            uniform("previousBones",bones.data(),sizeof(bones));
            const float params[4]={float(skinned),1,0,0};
            const float offsets[4]={jitter.x/kSize,jitter.y/kSize,previousJitter.x/kSize,previousJitter.y/kSize};
            uniform("motionParams",params,sizeof(params));
            uniform("motionJitter",offsets,sizeof(offsets));
            if(cutout) {
                const float color[4]={1,1,1,1}, opacity[4]={1,0,0,0}, cutoff[4]={.5f,0,0,0}, source[4]={0,0,0,0};
                motion.setTexture(0,motion.getTextureBinding("albedoMap"),toShaderTexture(white));
                motion.setTexture(1,motion.getTextureBinding("opacityMap"),toShaderTexture(white));
                uniform("baseColor",color,sizeof(color));
                uniform("opacity",opacity,sizeof(opacity));
                uniform("alphaCutoff",cutoff,sizeof(cutoff));
                uniform("opacitySource",source,sizeof(source));
            }
            fixture.adapter.setStateDepthTestAlways();
            motion.submit({0,0});
            const auto pixels=fixture.readTarget();
            float error=0; bool valid=true;
            for(int y=10;y<22;++y) for(int x=10;x<22;++x) {
                const auto& p=pixels[y*kSize+x];
                const float previousX=(2*(x+.5f-jitter.x)/kSize-1)-(moving?4.0f/kSize:0);
                const float expectedDepth=.5f+(sloped?.16f:0)*previousX;
                valid=valid && std::abs(p[3]-(sloped?1.01f:1.0f))<.0001f
                    && std::abs(p[2]-expectedDepth)<.0001f;
                error=std::max(error,std::max(std::abs(p[0]*kSize-(moving?2.0f:0)),std::abs(p[1]*kSize)));
            }
            CHECK(valid);
            CHECK(error < .001f);
            if(moving) worstMoving=std::max(worstMoving,error);
            else worstStatic=std::max(worstStatic,error);
        }
        std::cout << "[Motion GPU] cutout=" << cutout << " staticErrorPixels=" << worstStatic
                  << " movingErrorPixels=" << worstMoving << '\n';
    }
    bgfx::destroy(vb);
#endif
}

TEST_SUITE_END

TEST_SUITE(AYRenderer_TAAResidentGpu)

TEST_CASE(taa_gpu_resident_ping_pong_uses_immediately_previous_frame)
{
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend) { std::cout << "[SKIP] Isolated resident-history GPU suite requires AY_TAA_GPU_TEST.\n"; return; }
    TaaGpuFixture fixture;
    const bool ready=fixture.initialize(backend,true);
    CHECK(ready);
    if(!ready) return;
    constexpr int frames=40;
    // Keep both RGBA16F histories on GPU. No per-frame readback, CPU history
    // upload or extra empty frame: this exercises actual queued ping-pong.
    const auto second=fixture.adapter.createFrameBuffer(kSize,kSize,bgfx::TextureFormat::RGBA16F,false,true);
    CHECK(bgfx::isValid(second));
    if(!bgfx::isValid(second)) return;
    const std::array<bgfx::FrameBufferHandle,2> targets={fixture.target,second};
    const std::array<bgfx::TextureHandle,2> colors={fixture.targetTexture,fixture.adapter.getFboAttachment(second,0)};
    std::vector<Pixel> checker(kSize*kSize);
    for(int y=0;y<kSize;++y) for(int x=0;x<kSize;++x) {
        const float c=float((x+y)%2);
        checker[y*kSize+x]={c,c,c,1};
    }
    // The checker provides a wide clipping box. A distinct bootstrap image
    // gives a known .75*.92^N recurrence at black pixels, exposing split
    // odd/even histories, stale targets and accidental history resets.
    const auto seed=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{.75f,.75f,.75f,1}),true,true);
    const auto current=fixture.input(checker,false,true);
    const auto world=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{0,0,.2f,1}),true);
    const auto surface=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{0,0,0,1}),true,true);
    const auto velocity=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{0,0,.2f,1}),true);
    const auto depth=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{.2f,0,0,0}),true);
    std::array<bgfx::TextureHandle,frames> snapshots;
    for(auto& snapshot:snapshots) {
        snapshot=bgfx::createTexture2D(kSize,kSize,false,1,bgfx::TextureFormat::RGBA16F,
            BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST);
        CHECK(bgfx::isValid(snapshot));
        if(bgfx::isValid(snapshot)) fixture.textures.push_back(snapshot);
    }
    for(int frame=0;frame<frames;++frame) {
        const unsigned write=frame%2, read=1-write;
        CHECK(colors[write].idx!=colors[read].idx);
        const std::array<bgfx::TextureHandle,6> bindings={frame?current:seed,colors[read],world,surface,velocity,depth};
        fixture.submitResolve({},bindings,frame!=0,0,targets[write]);
        bgfx::blit(1,snapshots[frame],0,0,colors[write]);
        fixture.adapter.endFrame();
    }
    std::array<std::vector<uint16_t>,frames> pixels;
    uint32_t lastReady=0;
    for(int frame=0;frame<frames;++frame) {
        pixels[frame].resize(kSize*kSize*4);
        lastReady=std::max(lastReady,bgfx::readTexture(snapshots[frame],pixels[frame].data()));
    }
    for(int i=0;i<120 && fixture.adapter.gpuFrameCounter()<lastReady;++i) fixture.adapter.endFrame();
    CHECK(fixture.adapter.gpuFrameCounter()>=lastReady);
    float worstError=0;
    for(int frame=0;frame<frames;++frame) {
        const size_t center=(16*kSize+16)*4;
        const float actual=bx::halfToFloat(pixels[frame][center]);
        const float expected=(191.0f/255.0f)*std::pow(.92f,float(frame));
        worstError=std::max(worstError,std::abs(actual-expected));
        CHECK(std::abs(actual-expected)<.004f);
        CHECK(std::abs(bx::halfToFloat(pixels[frame][center+3])-.2f)<.001f);
    }
    std::cout << "[TAA resident] frames=" << frames << " maxRecurrenceError=" << worstError << '\n';
    fixture.adapter.destroy(second);
#endif
}

TEST_SUITE_END

TEST_SUITE(AYRenderer_TAAEdgeGpu)

TEST_CASE(taa_static_sloped_surface_retains_history_without_accepting_disocclusion)
{
#ifdef _WIN32
    const char* backend=std::getenv("AY_TAA_GPU_TEST");
    if(!backend) { std::cout << "[SKIP] GPU slope diagnostic requires AY_TAA_GPU_TEST.\n"; return; }
    TaaGpuFixture fixture;
    const bool ready=fixture.initialize(backend,true);
    CHECK(ready);
    if(!ready) return;
    float flatPeak=0;
    for(int scenario=0;scenario<3;++scenario) {
        const float slope=scenario==0 ? 0.0f : .01f;
        const bool footprintEnabled=scenario!=1; // No-footprint control reproduces v8.
        std::vector<Pixel> history(kSize*kSize,Pixel{0,0,0,-1});
        std::vector<float> low(kSize*kSize,1),high(kSize*kSize,0);
        unsigned rejected=0,checked=0;
        for(int frame=0;frame<24;++frame) {
            const auto jitter=taaHaltonJitter(frame%8);
            std::vector<Pixel> current(kSize*kSize),world(kSize*kSize),surface(kSize*kSize),velocity(kSize*kSize);
            for(int y=0;y<kSize;++y) for(int x=0;x<kSize;++x) {
                const size_t i=y*kSize+x;
                const bool foreground=x+.5f-jitter.x > 16.55f + .13f*(y+.5f-jitter.y-16);
                const float z=foreground ? .2f+slope*(x+.5f-jitter.x) : .9f;
                current[i]=foreground ? Pixel{1,0,0,1} : Pixel{0,0,1,1};
                world[i]={0,0,z,1}; surface[i]={0,0,0,1};
                velocity[i]={0,0,z,1+(foreground && footprintEnabled?slope:0)};
            }
            if(frame>=16) {
                const auto weights=fixture.dispatch(jitter,current,history,world,surface,velocity,true,2);
                for(int y=8;y<24;++y) for(int x=16;x<21;++x) {
                    ++checked;
                    if(weights[y*kSize+x][0]<.01f) ++rejected;
                }
            }
            history=fixture.dispatch(jitter,current,history,world,surface,velocity,frame!=0);
            if(frame>=16) for(size_t i=0;i<history.size();++i) {
                low[i]=std::min(low[i],history[i][0]); high[i]=std::max(high[i],history[i][0]);
            }
        }
        float peak=0;
        for(int y=8;y<24;++y) for(int x=16;x<21;++x) peak=std::max(peak,high[y*kSize+x]-low[y*kSize+x]);
        std::cout << "[TAA slope] gradient=" << slope << " footprint=" << footprintEnabled
                  << " rejected=" << rejected << '/' << checked << " peak=" << peak << '\n';
        if(scenario==0) flatPeak=peak;
        if(scenario==2) { CHECK(peak<=flatPeak+.003f); }
        // Small silhouette-coverage changes may reject history; a stationary
        // sloped face must not lose half its history merely due to jitter.
        if(footprintEnabled) { CHECK(rejected<=8u); }
        else { CHECK(rejected>checked/4); }
        std::vector<Pixel> current(kSize*kSize,Pixel{0,0,1,1});
        std::vector<Pixel> world(kSize*kSize,Pixel{0,0,.5f,1});
        std::vector<Pixel> surface(kSize*kSize,Pixel{0,0,0,1});
        std::vector<Pixel> velocity(kSize*kSize,Pixel{0,0,.5f,1+slope});
        std::vector<Pixel> occluded(kSize*kSize,Pixel{1,0,0,.47f});
        const auto weight=fixture.dispatch({},current,occluded,world,surface,velocity,true,2);
        CHECK(weight[16*kSize+16][0]==0);
        const auto resolved=fixture.dispatch({},current,occluded,world,surface,velocity,true);
        CHECK(resolved[16*kSize+16][0]<.001f);
        CHECK(resolved[16*kSize+16][2]>.999f);
        // Even a large footprint cannot legitimize a point outside the old
        // depth clip range. Missing motion remains invalid as well.
        for(auto& v:velocity) v={0,0,-.1f,1};
        const auto clipped=fixture.dispatch({},current,occluded,world,surface,velocity,true,2);
        CHECK(clipped[16*kSize+16][0]==0);
        for(auto& v:velocity) v={0,0,.47f,2};
        const auto extreme=fixture.dispatch({},current,occluded,world,surface,velocity,true,2);
        CHECK(extreme[16*kSize+16][0]==0);
        for(auto& v:velocity) v={0,0,.47f,0};
        const auto missing=fixture.dispatch({},current,occluded,world,surface,velocity,true,2);
        CHECK(missing[16*kSize+16][0]==0);
    }
    // Depth provenance: history alpha must use sampled raster depth rather
    // than reconstructing depth from an imprecise world-position attachment.
    const auto white=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{1,1,1,1}),true,true);
    const auto incorrectWorld=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{0,0,.45f,1}),true);
    const auto motion=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{0,0,.5f,1}),true);
    const auto depth=fixture.input(std::vector<Pixel>(kSize*kSize,Pixel{.5f,0,0,0}),true);
    fixture.submitResolve({}, {white,white,incorrectWorld,white,motion,depth},false,0,fixture.target);
    const auto raster=fixture.readTarget();
    CHECK(std::abs(raster[16*kSize+16][3]-.5f)<.001f);
#endif
}

TEST_CASE(taa_recursive_hard_edges_converge_without_surface_class_flicker)
{
#ifdef _WIN32
    const char* backend = std::getenv("AY_TAA_GPU_TEST");
    if (!backend) { std::cout << "[SKIP] Isolated AYRenderer_TAAEdgeGpu requires AY_TAA_GPU_TEST.\n"; return; }
    TaaGpuFixture fixture;
    const bool ready = fixture.initialize(backend, true);
    CHECK(ready);
    if (!ready) return;
    for (int scenario = 0; scenario < 3; ++scenario) {
        std::vector<Pixel> history(kSize*kSize, Pixel{0,0,0,-1});
        std::vector<Pixel> low(kSize*kSize, Pixel{1,1,1,1}), high(kSize*kSize, Pixel{0,0,0,0});
        std::array<std::vector<Pixel>, 8> finalCycle;
        for (int frame = 0; frame < 96; ++frame) {
            const auto jitter = taaHaltonJitter(frame % 8);
            std::vector<Pixel> current(kSize*kSize), world(kSize*kSize), surface(kSize*kSize), velocity(kSize*kSize);
            for (int y = 0; y < kSize; ++y) for (int x = 0; x < kSize; ++x) {
                const float slope = scenario == 0 ? 0.0f : 0.41421356f;
                const float distance = x + .5f - jitter.x - 16.55f - slope*(y + .5f - jitter.y - 16.5f);
                const bool foreground = scenario == 2 ? std::abs(distance) < .4f : distance >= 0;
                const size_t i = y*kSize+x;
                current[i] = foreground ? Pixel{1,0,0,1} : Pixel{0,0,1,1};
                world[i] = {0,0,.2f,1};
                surface[i] = {0,0,0,foreground ? 1.0f : 0.0f};
                velocity[i] = {0,0,.2f,foreground ? 1.0f : 0.0f};
            }
            // Crucially feed the GPU result, not a fabricated ideal history,
            // into the next frame. Use production RGBA8 + RGBA16F precision.
            history = fixture.dispatch(jitter, current, history, world, surface, velocity, frame != 0);
            if (frame >= 88) finalCycle[frame - 88] = history;
            if (frame >= 64) for (size_t i = 0; i < history.size(); ++i)
                for (int c = 0; c < 3; ++c) { low[i][c] = std::min(low[i][c], history[i][c]); high[i][c] = std::max(high[i][c], history[i][c]); }
        }
        float peakToPeak = 0;
        size_t worstPixel = 0;
        for (int y = 8; y < 24; ++y) for (int x = 8; x < 24; ++x)
            for (int c = 0; c < 3; ++c) {
                const float difference = high[y*kSize+x][c]-low[y*kSize+x][c];
                if (difference > peakToPeak) { peakToPeak = difference; worstPixel = y*kSize+x; }
            }
        std::cout << "[TAA recursive edge] scenario=" << scenario << " peak-to-peak=" << peakToPeak << '\n';
        std::cout << "[TAA worst pixel] x=" << worstPixel%kSize << " y=" << worstPixel/kSize;
        for (const auto& phase : finalCycle) std::cout << " (R=" << phase[worstPixel][0] << ",depth=" << phase[worstPixel][3] << ')';
        std::cout << '\n';
        CHECK(peakToPeak < 0.10f);
        // Red/blue coverage must conserve energy and cannot invent green.
        for (const auto& phase : finalCycle) {
            float energyError = 0, greenLeak = 0;
            for (const auto& pixel : phase) {
                energyError = std::max(energyError, std::abs(pixel[0]+pixel[2]-1));
                greenLeak = std::max(greenLeak, std::abs(pixel[1]));
            }
            CHECK(energyError < .005f);
            CHECK(greenLeak < .002f);
        }
    }

    // Saturated colors detect an incorrect YCoCg inverse that grayscale
    // cannot: (Y-Co)-Cg must not be parsed as Y-(Co-Cg).
    const size_t center = 16*kSize+16;
    std::vector<Pixel> world(kSize*kSize, Pixel{0,0,.2f,1});
    std::vector<Pixel> surface(kSize*kSize, Pixel{0,0,0,1});
    std::vector<Pixel> velocity(kSize*kSize, Pixel{0,0,.2f,1});
    for (const Pixel color : {Pixel{1,0,0,1}, Pixel{0,1,0,1}, Pixel{0,0,1,1}, Pixel{.25f,.5f,.75f,1}}) {
        std::vector<Pixel> current(kSize*kSize, color), history=current;
        for (auto& p : history) p[3] = .2f;
        for (int frame=0; frame<16; ++frame)
            history = fixture.dispatch(taaHaltonJitter(frame%8), current, history, world, surface, velocity, true);
        for (int c=0; c<3; ++c) CHECK(std::abs(history[center][c]-color[c]) < .004f);
    }

    // Translate a red strip by 8 pixels. Its matching old surface is retained;
    // the uncovered region rejects old foreground immediately (no red trail).
    std::vector<Pixel> current(kSize*kSize, Pixel{0,0,1,1});
    std::vector<Pixel> history(kSize*kSize, Pixel{0,0,1,-1});
    auto strip = [&](int left, float movement) {
        for (int y=0; y<kSize; ++y) for (int x=0; x<kSize; ++x) {
            const size_t i=y*kSize+x;
            const bool foreground=x>=left && x<left+5;
            current[i]=foreground ? Pixel{1,0,0,1} : Pixel{0,0,1,1};
            surface[i]={0,0,0,foreground ? 1.0f : 0.0f};
            velocity[i]={movement/kSize,0,.2f,foreground ? 1.0f : 0.0f};
        }
    };
    strip(8, 0);
    history=fixture.dispatch({},current,history,world,surface,velocity,false);
    strip(16, 8);
    const auto moved=fixture.dispatch({},current,history,world,surface,velocity,true);
    const size_t uncovered=16*kSize+10, moving=16*kSize+18;
    CHECK(moved[uncovered][0] < .002f);
    CHECK(moved[uncovered][2] > .998f);
    CHECK(moved[moving][0] > .998f);
    const auto rejection=fixture.dispatch({},current,history,world,surface,velocity,true,1);
    CHECK(rejection[uncovered][0] > .998f);
    CHECK(rejection[moving][1] > .998f);
    const auto feedback=fixture.dispatch({},current,history,world,surface,velocity,true,2);
    CHECK(feedback[uncovered][0] < .002f);
    CHECK(std::abs(feedback[moving][0]-.785f) < .002f);
    const auto motion=fixture.dispatch({},current,history,world,surface,velocity,true,4);
    CHECK(motion[moving][0] > .998f);
    CHECK(std::abs(motion[moving][1]-.5f) < .002f);
    const auto reprojected=fixture.dispatch({},current,history,world,surface,velocity,true,5);
    CHECK(reprojected[moving][0] > .998f);
    const auto reset=fixture.dispatch({},current,history,world,surface,velocity,false,2);
    CHECK(reset[moving][0] < .002f);
    // A whole-field change must be clipped; diagnostics cannot alter inputs.
    for(auto& pixel:current) pixel={0,1,0,1};
    const auto clipping=fixture.dispatch({},current,history,world,surface,velocity,true,3);
    CHECK(clipping[moving][0] > .99f);
    const auto changed=fixture.dispatch({},current,history,world,surface,velocity,true);
    CHECK(changed[moving][1] > .998f);
#endif
}

TEST_SUITE_END
