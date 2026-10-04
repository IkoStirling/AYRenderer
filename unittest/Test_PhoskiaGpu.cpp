#include "AYTest.h"
#include "AYShader/ShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/GpuResources.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
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

    std::vector<Pixel> render(const std::string& declarations, const std::string& body,
        const std::function<void(const ayt::shader::ShaderResource&)>& bind = {}) {
        const auto source = "material Probe { " + declarations +
            " vertex { in position : position; return vec4(position, 1.0) } fragment { " + body + " } }";
        auto program = pool.acquire(source);
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

TEST_SUITE_END
