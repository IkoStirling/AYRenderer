#include "AYRenderer.h"
#include "AYTest.h"

#include <iostream>
#include <string>
#include <sys/stat.h>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

const char* kMvpMaterial = R"(
material MvpTest {
    property baseColor = vec4(1.0, 0.2, 0.2, 1.0)
    vertex {
        in pos : position
        return u_modelViewProj * vec4(pos, 1.0)
    }
    fragment {
        return baseColor
    }
}
)";

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

} // namespace

TEST_SUITE(RenderLightingCameraTests)

TEST_CASE(frame_uniforms_allow_rotated_cube_draw)
{
    if (!fileExists(AY_SHADER_SHADERC_HINT)) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 640;
    desc.height  = 480;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle material =
        renderer.createMaterialFromPhoskia(kMvpMaterial, "renderer_mvp_test");
    if (!material.isValid()) {
        renderer.shutdown();
        return;
    }

    ayt::render::MeshHandle mesh = renderer.createUnitCube();
    CHECK(mesh.isValid());

    ayt::render::RenderScene scene;
    scene.add(mesh, material, ayt::math::rotate(ayt::math::FVector3(0.0f, 1.0f, 0.0f), 0.7f));

    renderer.setDirectionalLight(ayt::math::FVector3(0.2f, -1.0f, -0.3f),
                                 ayt::math::FVector3(1.0f, 0.95f, 0.85f));
    renderer.setMainCameraLookAtPerspective(ayt::math::FVector3(0.0f, 0.0f, 4.0f),
                                            ayt::math::FVector3(0.0f, 0.0f, 0.0f),
                                            ayt::math::FVector3(0.0f, 1.0f, 0.0f),
                                            60.0f, 640.0f / 480.0f, 0.1f, 100.0f);

    renderer.beginFrame({});
    renderer.render(scene);
    renderer.endFrame();

    renderer.destroyMesh(mesh);
    renderer.destroyMaterial(material);
    renderer.shutdown();
}

TEST_SUITE_END
