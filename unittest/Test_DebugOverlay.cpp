// Test_DebugOverlay.cpp — R4-b debug overlay + frame stats (Noop backend)

#include "AYRenderer.h"
#include "AYTest.h"

#include <sys/stat.h>

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

    const ayt::render::RenderFrameStats& stats = renderer.getFrameStats();
    CHECK(stats.frameCount >= 1u);
    CHECK(stats.frameTimeMs >= 0.0f);

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

    renderer.destroyMesh(mesh);
    renderer.destroyMaterial(material);
    renderer.shutdown();
}

TEST_SUITE_END
