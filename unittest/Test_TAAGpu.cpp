#include "AYTest.h"
#include "detail/TAAPass.h"
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
            BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
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
        const std::array<const char*, 6> names = {
            "currentColor", "historyColor", "worldPosition", "geometryData", "motionVectors", "sceneDepth"};
        const std::array<const std::vector<Pixel>*, 6> inputs = {
            &current, &history, &world, &surface, &velocity, &sceneDepth};
        const auto firstInput = textures.size();
        configureFullscreenPassView(adapter, 0, target, 0, 0, kSize, kSize);
        geometry.bind(adapter);
        for (uint8_t i = 0; i < inputs.size(); ++i) {
            const auto texture = input(*inputs[i], i >= 2, i == 0 || i == 3);
            CHECK(bgfx::isValid(texture));
            program.setTexture(i, program.getTextureBinding(names[i]), toShaderTexture(texture));
        }
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
        bgfx::blit(1, readback, 0, 0, targetTexture);
        std::vector<Pixel> result(current.size());
        std::vector<uint16_t> halves(result.size() * 4);
        const auto ready = bgfx::readTexture(readback,
            productionPrecision ? static_cast<void*>(halves.data()) : static_cast<void*>(result.data()));
        for (int i = 0; i < 120 && adapter.gpuFrameCounter() < ready; ++i) adapter.endFrame();
        CHECK(adapter.gpuFrameCounter() >= ready);
        if (productionPrecision) {
            for (size_t i = 0; i < result.size(); ++i)
                for (size_t c = 0; c < 4; ++c) result[i][c] = bx::halfToFloat(halves[i * 4 + c]);
        }
        for (auto i = firstInput; i < textures.size(); ++i) bgfx::destroy(textures[i]);
        textures.resize(firstInput);
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

TEST_SUITE(AYRenderer_TAAEdgeGpu)

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
