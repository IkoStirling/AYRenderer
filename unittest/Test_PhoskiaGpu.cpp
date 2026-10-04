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

#include <array>
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
