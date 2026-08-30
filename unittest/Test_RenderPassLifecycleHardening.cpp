#include "AYTest.h"

#include <array>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string readRendererSource(const char* relativePath)
{
    std::ifstream file(std::string(AY_RENDERER_SOURCE_DIR) + "/" + relativePath);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

TEST_SUITE(RenderPassLifecycleHardening)

TEST_CASE(shutdown_releases_every_pass_owned_gpu_resource_before_adapter)
{
    const std::string source = readRendererSource("src/AYRenderer.cpp");
    const std::size_t shutdown = source.find("void Renderer::shutdown()");
    const std::size_t adapterShutdown =
        source.find("_impl->adapter.shutdown()", shutdown);
    CHECK(shutdown != std::string::npos);
    CHECK(adapterShutdown != std::string::npos);

    constexpr std::array<const char*, 14> passNames = {
        "Transparent", "Shadow", "GBuffer", "Lighting", "Skybox",
        "BloomExtract", "BloomBlur", "DepthHaze", "GBufferDebug",
        "PostProcess", "FXAA", "ColorGrading", "Present", "SSAO",
    };
    for (const char* passName : passNames) {
        const std::string lookupText =
            std::string("_impl->pipeline.findPass(\"") + passName + "\")";
        const std::size_t lookup = source.find(lookupText, shutdown);
        const std::size_t destroy = source.find(
            "->destroyResources(_impl->adapter)", lookup);
        CHECK(lookup != std::string::npos);
        CHECK(destroy != std::string::npos);
        CHECK(lookup < destroy);
        CHECK(destroy < adapterShutdown);
    }
}

TEST_CASE(reinitialize_replaces_the_shutdown_hot_reload_token)
{
    const std::string source = readRendererSource("src/AYRenderer.cpp");
    const std::size_t initialize =
        source.find("bool Renderer::initialize(const InitDesc& desc)");
    const std::size_t recreate = source.find(
        "_impl->aliveToken = std::make_shared<std::atomic<bool>>(true);",
        initialize);
    const std::size_t capture =
        source.find("auto aliveToken = _impl->aliveToken;", initialize);
    CHECK(initialize != std::string::npos);
    CHECK(recreate != std::string::npos);
    CHECK(capture != std::string::npos);
    CHECK(recreate < capture);
}

TEST_CASE(color_only_framebuffer_failure_releases_the_unowned_texture)
{
    const std::string source =
        readRendererSource("src/detail/BGFXAdapter.cpp");
    const std::size_t function = source.find(
        "bgfx::FrameBufferHandle BGFXAdapter::createFrameBuffer(");
    const std::size_t create = source.find(
        "bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(", function);
    const std::size_t success = source.find("if (bgfx::isValid(fb))", create);
    const std::size_t cleanup = source.find("bgfx::destroy(color);", success);
    const std::size_t invalidReturn = source.find(
        "return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};", cleanup);
    CHECK(function != std::string::npos);
    CHECK(create != std::string::npos);
    CHECK(success != std::string::npos);
    CHECK(cleanup != std::string::npos);
    CHECK(invalidReturn != std::string::npos);
    CHECK(success < cleanup);
    CHECK(cleanup < invalidReturn);
}

TEST_CASE(fullscreen_buffer_creation_is_transactional_and_shared_by_all_passes)
{
    const std::string helper = readRendererSource("src/detail/RenderPass.h");
    const std::size_t function =
        helper.find("inline bool ensureFullscreenTriangleBuffers(");
    const std::size_t newVertex =
        helper.find("newVertexBuffer", function);
    const std::size_t newIndex =
        helper.find("newIndexBuffer", newVertex);
    const std::size_t partialCleanup =
        helper.find("adapter.destroy(newVertexBuffer);", newIndex);
    const std::size_t publishVertex =
        helper.find("vertexBuffer = newVertexBuffer;", partialCleanup);
    const std::size_t publishIndex =
        helper.find("indexBuffer = newIndexBuffer;", publishVertex);
    CHECK(function != std::string::npos);
    CHECK(newVertex != std::string::npos);
    CHECK(newIndex != std::string::npos);
    CHECK(partialCleanup != std::string::npos);
    CHECK(publishVertex != std::string::npos);
    CHECK(publishIndex != std::string::npos);

    constexpr std::array<const char*, 7> directHelperPassFiles = {
        "src/detail/LightingPass.cpp",
        "src/detail/SkyboxPass.cpp",
        "src/detail/BloomExtractPass.cpp",
        "src/detail/BloomBlurPass.cpp",
        "src/detail/DepthHazePass.cpp",
        "src/detail/SSAOPass.cpp",
        "src/detail/GBufferDebugPass.cpp",
    };
    for (const char* passFile : directHelperPassFiles) {
        const std::string source = readRendererSource(passFile);
        CHECK(source.find("ensureFullscreenTriangleBuffers(")
              != std::string::npos);
    }

    // FinalPP, FXAA, ColorGrading and Present use the typed wrapper so the
    // triangle data, view setup and destruction contract live in one place.
    // The wrapper itself must still delegate allocation to the transactional
    // helper above.
    const std::string geometry =
        readRendererSource("src/detail/FullscreenPassGeometry.cpp");
    CHECK(geometry.find("ensureFullscreenTriangleBuffers(")
          != std::string::npos);

    const std::string postProcess =
        readRendererSource("src/detail/PostProcessPass.cpp");
    const std::string present =
        readRendererSource("src/detail/PresentPass.cpp");
    const std::string fxaa =
        readRendererSource("src/detail/FXAAPass.cpp");
    const std::string colorGrading =
        readRendererSource("src/detail/ColorGradingPass.cpp");
    CHECK(postProcess.find("_fullscreen.ensure(adapter)")
          != std::string::npos);
    CHECK(present.find("_geometry.ensure(adapter)")
          != std::string::npos);
    CHECK(fxaa.find("_geometry.ensure(adapter)")
          != std::string::npos);
    CHECK(colorGrading.find("_geometry.ensure(adapter)")
          != std::string::npos);
}

TEST_SUITE_END
