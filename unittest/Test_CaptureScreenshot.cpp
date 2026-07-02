// Test_CaptureScreenshot.cpp — R4-c captureScreenshot API guards

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

TEST_SUITE(RendererCaptureScreenshotTests)

TEST_CASE(capture_screenshot_rejects_invalid_requests)
{
    ayt::render::Renderer renderer;

    CHECK(!renderer.captureScreenshot(""));
    CHECK(!renderer.captureScreenshot("shot.png"));

    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 640;
    desc.height  = 480;
    CHECK(renderer.initialize(desc));
    CHECK(!renderer.captureScreenshot("shot.png"));

    renderer.shutdown();
}

TEST_CASE(capture_screenshot_queues_without_crash_on_noop_frame)
{
    if (!shadercAvailable()) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle material =
        renderer.createMaterialFromPhoskia(kUnlitMaterial, "capture_unlit");
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
    CHECK(!renderer.captureScreenshot("noop_shot.png"));
    renderer.endFrame();

    CHECK(!fileExists("noop_shot.png"));

    renderer.destroyMesh(mesh);
    renderer.destroyMaterial(material);
    renderer.shutdown();
}

TEST_SUITE_END
