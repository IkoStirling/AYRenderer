// Test_DebugOverlay.cpp — R4-b debug overlay + frame stats (Noop backend)

#include "AYRenderer.h"
#include "AYTest.h"
#include "detail/DebugOverlay.h"

#include <sys/stat.h>

#include <algorithm>
#include <iostream>
#include <string>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

const char* kUnlitMaterial = R"(
material Unlit {
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0);

    vertex {
        in  position : position;
        out position : position;
        return vec4(position, 1.0);
    }
    fragment {
        in  position : position;
        return baseColor;
    }
}
)";

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool shadercAvailable()
{
    return fileExists(AY_SHADER_SHADERC_HINT);
}

} // namespace

TEST_SUITE(RendererDebugOverlayTests)

TEST_CASE(debug_overlay_toggle_and_frame_stats)
{
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend             = ayt::render::Backend::Noop;
    desc.width               = 640;
    desc.height              = 480;
    desc.enableDebugOverlay  = true;

    CHECK(renderer.initialize(desc));
    CHECK(renderer.isDebugOverlayEnabled());

    renderer.setDebugOverlayEnabled(false);
    CHECK(!renderer.isDebugOverlayEnabled());
    renderer.setDebugOverlayEnabled(true);
    CHECK(renderer.isDebugOverlayEnabled());

    ayt::render::ClearDesc clear;
    renderer.beginFrame(clear);
    renderer.endFrame();
    renderer.beginFrame(clear);
    renderer.endFrame();

    const ayt::render::RenderFrameStats& stats = renderer.getFrameStats();
    CHECK(stats.frameCount >= 2u);
    CHECK(stats.frameTimeMs >= 0.0f);
    CHECK(stats.avgFrameTimeMs >= 0.0f);
    CHECK(stats.p95FrameTimeMs >= stats.avgFrameTimeMs);
    CHECK(stats.p99FrameTimeMs >= stats.p95FrameTimeMs);
    CHECK(stats.fps >= 0.0f);

    renderer.shutdown();
}

TEST_CASE(debug_overlay_reports_draw_count)
{
    if (!shadercAvailable()) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend            = ayt::render::Backend::Noop;
    desc.width              = 800;
    desc.height             = 600;
    desc.enableDebugOverlay = true;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle material =
        renderer.createMaterialFromPhoskia(kUnlitMaterial, "debug_overlay_unlit");
    if (!material.isValid()) {
        std::cerr << "[Renderer test] SKIP: material acquire failed.\n";
        renderer.shutdown();
        return;
    }

    ayt::render::MeshHandle mesh = renderer.createUnitCube();
    CHECK(mesh.isValid());

    ayt::render::RenderScene scene;
    scene.add(mesh, material);

    renderer.beginFrame({});
    renderer.render(scene);
    renderer.endFrame();

    const ayt::render::RenderFrameStats& stats = renderer.getFrameStats();
    CHECK(stats.drawCalls == 1u);
    CHECK(stats.sceneItems == 1u);
    CHECK(stats.frameCount >= 1u);

    const auto pass = std::find_if(stats.passes.begin(), stats.passes.end(),
        [](const ayt::render::RenderPassFrameStats& item) {
            return item.name == "ForwardOpaque";
        });
    CHECK(pass != stats.passes.end());
    if (pass != stats.passes.end()) {
        CHECK(pass->drawCalls == 1u);
        CHECK(pass->cpuTimeMs >= 0.0f);
        CHECK(pass->gpuTimeMs >= 0.0f);
    }

    renderer.destroyMesh(mesh);
    renderer.destroyMaterial(material);
    renderer.shutdown();
}

TEST_CASE(noop_backend_graph_culls_gpu_postprocess_but_keeps_cpu_visible_passes)
{
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend            = ayt::render::Backend::Noop;
    desc.width              = 640;
    desc.height             = 480;
    desc.enableDebugOverlay = true;
    CHECK(renderer.initialize(desc));
    renderer.configurePipeline(ayt::render::RenderPipelineDesc::makeDeferred());

    const ayt::render::RenderScene emptyScene;
    renderer.beginFrame({});
    renderer.render(emptyScene);
    renderer.endFrame();

    const ayt::render::RenderFrameStats& stats = renderer.getFrameStats();
    CHECK(stats.sceneItems == 0u);
    CHECK(!stats.passes.empty());

    const auto dispatched = [&stats](const char* name) {
        return std::any_of(stats.passes.begin(), stats.passes.end(),
                           [name](const ayt::render::RenderPassFrameStats& pass) {
                               return pass.name == name;
                           });
    };
    CHECK(dispatched("GBuffer"));
    CHECK(dispatched("Lighting"));
    // Noop has no usable render target. The compiled FrameGraph now owns the
    // execution decision, so PostProcess is graph-culled instead of entering
    // execute() only to return zero.
    CHECK(!dispatched("PostProcess"));
    CHECK(dispatched("UI"));
    CHECK(stats.graph.compiled);
    CHECK(stats.graph.compileSucceeded);
    CHECK(stats.graph.declaredPasses > 0u);
    CHECK(stats.graph.livePasses > 0u);
    CHECK(stats.resources.declaredResources > 0u);

    renderer.shutdown();
}

TEST_CASE(nonzero_gpu_timings_survive_cpu_merge_and_map_to_current_passes)
{
    ayt::render::detail::DebugOverlay overlay;
    std::vector<ayt::render::RenderPassFrameStats> cpu;
    const char* names[] = {"Shadow", "MotionVector", "ColorGrading", "TAA",
        "Present", "FXAA", "SMAA", "EditorOverlay", "Transparent", "Forward2DOpaque"};
    for (const auto* name : names) cpu.push_back({name, 3u, 1.5f, 99.0f});
    const uint16_t ids[] = {18, 25, 3, 4, 5, 245, 16, 17, 247, 248, 249,
                           251, 252, 9, 253, 254, 246};
    std::vector<bgfx::ViewStats> views(std::size(ids));
    for (size_t i = 0; i < views.size(); ++i) {
        views[i].view = ids[i];
        views[i].gpuTimeBegin = 100;
        views[i].gpuTimeEnd = 102; // 2ms at 1000 ticks/sec.
    }
    bgfx::Stats gpu{};
    gpu.gpuTimerFreq = 1000;
    gpu.gpuTimeBegin = 100;
    gpu.gpuTimeEnd = 120;
    gpu.gpuFrameNum = 7;
    gpu.numViews = static_cast<uint16_t>(views.size());
    gpu.viewStats = views.data();
    overlay.updateFrameStats(30, 10, cpu, &gpu);
    const float expected[] = {4, 2, 2, 4, 2, 2, 6, 4, 6, 2};
    CHECK(overlay.stats().gpuFrameTimeMs == 20.0f);
    CHECK(overlay.stats().gpuFrameNumber == 7u);
    for (size_t i = 0; i < cpu.size(); ++i) {
        CHECK(overlay.stats().passes[i].gpuTimeMs == expected[i]);
        CHECK(overlay.stats().passes[i].cpuTimeMs == 1.5f);
        CHECK(overlay.stats().passes[i].drawCalls == 3u);
    }
    gpu.gpuTimerFreq = 0;
    overlay.updateFrameStats(0, 0, cpu, &gpu);
    for (const auto& pass : overlay.stats().passes) CHECK(pass.gpuTimeMs == 0.0f);
    overlay.updateFrameStats(0, 0, cpu, nullptr);
    CHECK(overlay.stats().gpuFrameTimeMs == 0.0f);
    for (const auto& pass : overlay.stats().passes) CHECK(pass.gpuTimeMs == 0.0f);
}

TEST_CASE(architecture_diagnostics_track_graph_resources_and_peak_targets)
{
    ayt::render::detail::DebugOverlay overlay;
    overlay.onBeginFrame();

    ayt::render::RenderGraphFrameStats graph{};
    graph.compiled = true;
    graph.compileSucceeded = true;
    graph.declaredPasses = 9;
    graph.livePasses = 7;
    graph.logicalResources = 8;
    graph.transientTargets = 4;
    graph.retainedTargets = 3;
    ayt::render::RenderResourceFrameStats resources{};
    resources.declaredResources = 12;
    resources.availableResources = 11;
    resources.validResources = 9;
    resources.producedResources = 7;
    resources.invalidResources = 3;
    resources.persistentHistoryResources = 2;
    overlay.setArchitectureStats(graph, resources);

    CHECK(overlay.stats().graph.compiled);
    CHECK(overlay.stats().graph.compileSucceeded);
    CHECK(overlay.stats().graph.livePasses == 7u);
    CHECK(overlay.stats().graph.peakTransientTargets == 4u);
    CHECK(overlay.stats().resources.producedResources == 7u);

    overlay.onBeginFrame();
    graph.transientTargets = 2;
    overlay.setArchitectureStats(graph, resources);
    CHECK(overlay.stats().graph.transientTargets == 2u);
    CHECK(overlay.stats().graph.peakTransientTargets == 4u);

    overlay.resetStats();
    CHECK(overlay.stats().graph.peakTransientTargets == 0u);
}

TEST_SUITE_END
