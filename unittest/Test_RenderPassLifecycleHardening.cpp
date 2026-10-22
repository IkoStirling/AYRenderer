#include "AYTest.h"
#include "AYRenderer/UIRenderBackend.h"
#include "detail/DeferredDecalPass.h"
#include "detail/GBufferPass.h"
#include "detail/RenderPipeline.h"
#include "detail/RenderPipelineFactory.h"
#include "detail/RenderPipelineLifecycle.h"
#include "detail/UIPass.h"

#include <array>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{

class DiagnosticGbufferName final : public ayt::render::detail::RenderPass {
public:
    std::string_view name() const override { return "GBuffer"; }
    uint32_t execute(ayt::render::detail::PassExecContext&) override { return 0; }
};

std::string readRendererSource(const char* relativePath)
{
    std::ifstream file(std::string(AY_RENDERER_SOURCE_DIR) + "/" + relativePath);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

TEST_SUITE(RenderPassLifecycleHardening)

TEST_CASE(pipeline_cleanup_preserves_instances_settings_and_borrowed_ui_without_name_casts)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    BGFXAdapter adapter;
    struct Cleanup { BGFXAdapter& adapter; ~Cleanup() { adapter.shutdown(); } } cleanup{adapter};
    RenderPipeline pipeline;
    pipeline.addPass(std::make_unique<DiagnosticGbufferName>());
    for (auto slot : {RenderPassSlot::GBuffer, RenderPassSlot::DeferredDecal,
        RenderPassSlot::MotionVector, RenderPassSlot::Transparent,
        RenderPassSlot::Lighting, RenderPassSlot::Shadow, RenderPassSlot::UI}) {
        pipeline.addPass(slot, makeRenderPass(slot));
    }
    auto* gbuffer = pipeline.findPass<GBufferPass>(); CHECK(gbuffer != nullptr);
    auto* ui = pipeline.findPass<UIPass>(); CHECK(ui != nullptr);
    if (!gbuffer || !ui) return;
    CHECK(pipeline.findPass("GBuffer") != gbuffer);
    UIRenderBackend backend; ui->setBackend(&backend);
    std::vector<RenderPass*> instances;
    std::vector<bool> enabled;
    for (const auto& pass : pipeline.passes()) {
        pass->setEnabled(instances.size()%2 == 0);
        instances.push_back(pass.get()); enabled.push_back(pass->isEnabled());
    }
    gbuffer->setGbufferSize(512,256);
    CHECK(!gbuffer->isEnabled()); // Disabled producers still require cleanup.
    destroyPipelineGpuResources(pipeline,adapter); // No live device: no cleanup.
    CHECK(gbuffer->gbufferWidth()==512); CHECK(gbuffer->gbufferHeight()==256);
    BGFXInitParams init; init.backend=Backend::Noop; init.width=init.height=16;
    const bool ready=adapter.initialize(init); CHECK(ready); if(!ready)return;
    destroyAttachmentBorrowerResources(pipeline,adapter);
    CHECK(gbuffer->gbufferWidth()==512); // Borrowers do not destroy the producer.
    destroyPipelineGpuResources(pipeline,adapter);
    CHECK(gbuffer->gbufferWidth()==0); CHECK(gbuffer->gbufferHeight()==0);
    CHECK(!bgfx::isValid(gbuffer->gbufferFbo()));
    CHECK(!bgfx::isValid(pipeline.findPass<DeferredDecalPass>()->output(1)));
    CHECK(pipeline.passes().size()==instances.size());
    for(size_t index=0;index<instances.size();++index) {
        CHECK(pipeline.passes()[index].get()==instances[index]);
        CHECK(pipeline.passes()[index]->isEnabled()==enabled[index]);
    }
    CHECK(ui->backend()==&backend); CHECK(!backend.isInitialized());
    destroyPipelineGpuResources(pipeline,adapter); // Idempotent, still mounted.
    CHECK(pipeline.passes().size()==instances.size()); CHECK(ui->backend()==&backend);
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
