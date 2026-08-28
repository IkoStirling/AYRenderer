#include "AYRenderer.h"
#include "AYTest.h"
#include "detail/CameraMath.h"
#include "detail/RasterConvention.h"

#include <cmath>
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
        return modelViewProjection * vec4(pos, 1.0)
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

TEST_CASE(lh_clockwise_front_faces_cull_counter_clockwise_backs)
{
    using namespace ayt::render::detail;

    CHECK(kCullBackFaces == BGFX_STATE_CULL_CCW);
    CHECK(kCullFrontFaces == BGFX_STATE_CULL_CW);
    CHECK((kCullBackFaces & BGFX_STATE_CULL_MASK) != 0u);
    CHECK((kCullFrontFaces & BGFX_STATE_CULL_MASK) != 0u);
    CHECK(kCullBackFaces != kCullFrontFaces);

    ayt::math::Float4x4 mirrored = ayt::math::Float4x4::identity();
    mirrored.row[0].x = -1.0f;
    CHECK_FALSE(reversesWinding(ayt::math::Float4x4::identity()));
    CHECK(reversesWinding(mirrored));
    CHECK(cullBackFacesForTransform(mirrored) == kCullFrontFaces);
    CHECK(cullFrontFacesForTransform(mirrored) == kCullBackFaces);
}

TEST_CASE(gbuffer_normal_matrix_handles_non_uniform_scale)
{
    ayt::math::Float4x4 model = ayt::math::Float4x4::identity();
    model.row[0].x = 2.0f;
    model.row[1].y = 4.0f;
    model.row[2].z = 5.0f;
    model.row[0].w = 7.0f;
    model.row[1].w = -3.0f;

    const ayt::math::Float4x4 normal =
        ayt::render::detail::normalMatrixForTransform(model);
    CHECK(std::abs(normal.row[0].x - 0.5f) < 1.0e-6f);
    CHECK(std::abs(normal.row[1].y - 0.25f) < 1.0e-6f);
    CHECK(std::abs(normal.row[2].z - 0.2f) < 1.0e-6f);

    ayt::math::Float4x4 singular = ayt::math::Float4x4::identity();
    singular.row[1].y = 0.0f;
    const ayt::math::Float4x4 fallback =
        ayt::render::detail::normalMatrixForTransform(singular);
    CHECK(std::abs(fallback.row[0].x - 1.0f) < 1.0e-6f);
    CHECK(std::abs(fallback.row[1].y - 1.0f) < 1.0e-6f);
    CHECK(std::abs(fallback.row[2].z - 1.0f) < 1.0e-6f);
    CHECK(std::abs(fallback.row[3].w - 1.0f) < 1.0e-6f);
}

TEST_CASE(camera_projection_converts_public_degrees_to_aymath_radians)
{
    constexpr float fovYDegrees = 50.0f;
    constexpr float aspect = 16.0f / 9.0f;

    const ayt::math::Float4x4 projection =
        ayt::render::detail::makeLeftHandedPerspectiveDegrees(
            fovYDegrees, aspect, 0.1f, 100.0f);
    const float expectedY = 1.0f / std::tan(
        ayt::math::radians(fovYDegrees) * 0.5f);

    CHECK(std::fabs(projection.row[1].y - expectedY) < 1.0e-5f);
    CHECK(std::fabs(projection.row[0].x - expectedY / aspect) < 1.0e-5f);
    CHECK(projection.row[0].x > 0.0f);
    CHECK(projection.row[1].y > 0.0f);
}

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
