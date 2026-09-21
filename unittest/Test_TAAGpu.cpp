#include "AYTest.h"
#include "detail/TAAPass.h"
#include "AYShader/ShaderResourcePool.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

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

    bool initialize(const char* backend) {
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
            bgfx::TextureFormat::RGBA32F, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(targetTexture)) return false;
        textures.push_back(targetTexture);
        target = bgfx::createFrameBuffer(1, &targetTexture, false);
        readback = bgfx::createTexture2D(kSize, kSize, false, 1,
            bgfx::TextureFormat::RGBA32F, BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
        if (!bgfx::isValid(readback)) return false;
        textures.push_back(readback);
        return bgfx::isValid(target);
    }

    bgfx::TextureHandle input(const std::vector<Pixel>& pixels, bool point) {
        const auto flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | (point ? BGFX_SAMPLER_POINT : 0);
        auto texture = bgfx::createTexture2D(kSize, kSize, false, 1,
            bgfx::TextureFormat::RGBA32F, flags,
            bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size() * sizeof(Pixel))));
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
        const std::array<const char*, 5> names = {
            "currentColor", "historyColor", "worldPosition", "geometryData", "motionVectors"};
        const std::array<const std::vector<Pixel>*, 5> inputs = {
            &current, &history, &world, &surface, &velocity};
        const auto firstInput = textures.size();
        configureFullscreenPassView(adapter, 0, target, 0, 0, kSize, kSize);
        geometry.bind(adapter);
        for (uint8_t i = 0; i < inputs.size(); ++i) {
            const auto texture = input(*inputs[i], i >= 2);
            CHECK(bgfx::isValid(texture));
            program.setTexture(i, program.getTextureBinding(names[i]), toShaderTexture(texture));
        }
        const float metrics[4] = {1.0f / kSize, 1.0f / kSize, kSize, kSize};
        const float params[4] = {0.92f, 0.65f, 0.025f, historyValid ? 1.0f : 0.0f};
        const float offset[4] = {jitter.x / kSize, jitter.y / kSize, 1, 1};
        const float depth[4] = {TAAPass::kHistoryDepthTolerance, 0, 0, 0};
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
        const auto ready = bgfx::readTexture(readback, result.data());
        for (int i = 0; i < 120 && adapter.gpuFrameCounter() < ready; ++i) adapter.endFrame();
        CHECK(adapter.gpuFrameCounter() >= ready);
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
