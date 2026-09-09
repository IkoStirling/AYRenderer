#include "detail/ShadowDebug.h"
#include "detail/ShadowMatrixBuilder.h"

#include "AYRenderer/RenderScene.h"
#include "AYMath/MathTypes.h"

#include "AYTest.h"

#include <cmath>
#include <unordered_map>

using ayt::math::Float4x4;
using ayt::math::FVector3;
using ayt::render::detail::ShadowSceneBounds;
using ayt::render::detail::ShadowProjectSample;
using ayt::render::detail::buildDirectionalShadowMatricesFromBounds;
using ayt::render::detail::computeShadowSceneBounds;
using ayt::render::detail::projectWorldThroughLvpColMajor;

namespace {

bool nearlyEqual(float a, float b, float eps = 0.02f)
{
    return std::fabs(a - b) <= eps;
}

ShadowSceneBounds editorPlayBounds()
{
    ShadowSceneBounds bounds{};
    bounds.valid  = true;
    bounds.min    = FVector3(-4.0f, -0.15f, -4.0f);
    bounds.max    = FVector3(4.0f, 1.35f, 4.0f);
    bounds.center = FVector3(0.0f, 0.45f, 0.0f);
    return bounds;
}

} // namespace

TEST_SUITE(ShadowMatrixBuilder)

TEST_CASE(scene_fit_ortho_maps_ground_depth_to_ndc01)
{
    const FVector3 lightDir(0.35f, -0.85f, -0.40f);

    Float4x4 view{};
    Float4x4 proj{};
    Float4x4 viewProj{};
    float viewCol[16] = {};
    float projCol[16] = {};
    float lvpCol[16]  = {};

    buildDirectionalShadowMatricesFromBounds(
        editorPlayBounds(),
        lightDir,
        view,
        proj,
        viewProj,
        viewCol,
        projCol,
        lvpCol);

    const ShadowProjectSample ground =
        projectWorldThroughLvpColMajor(lvpCol, FVector3(0.0f, 0.0f, 0.0f));
    const ShadowProjectSample cubeBottom =
        projectWorldThroughLvpColMajor(lvpCol, FVector3(0.0f, 0.35f, 0.0f));

    CHECK(ground.uvIn01);
    CHECK(ground.rawDepth >= 0.0f);
    CHECK(ground.rawDepth <= 1.0f);
    CHECK(ground.refDepth >= 0.0f);
    CHECK(ground.refDepth <= 1.0f);
    CHECK(ground.refDepth > 0.55f);
    CHECK(ground.refDepth < 0.95f);
    CHECK(cubeBottom.refDepth < ground.refDepth);
    CHECK(nearlyEqual(ground.shadowU, 0.50f, 0.08f));
}

TEST_CASE(scene_fit_depth_in_valid_clip_range_for_scene_points)
{
    const FVector3 lightDir(0.35f, -0.85f, -0.40f);

    Float4x4 view{};
    Float4x4 proj{};
    Float4x4 viewProj{};
    float viewCol[16] = {};
    float projCol[16] = {};
    float lvpCol[16]  = {};

    buildDirectionalShadowMatricesFromBounds(
        editorPlayBounds(),
        lightDir,
        view,
        proj,
        viewProj,
        viewCol,
        projCol,
        lvpCol);

    const FVector3 probes[] = {
        FVector3(0.0f, 0.85f, 0.0f),
        FVector3(0.0f, 0.35f, 0.0f),
        FVector3(0.0f, 0.0f, 0.0f),
        FVector3(3.0f, 0.0f, 3.0f),
    };
    for (const FVector3& p : probes) {
        const ShadowProjectSample s = projectWorldThroughLvpColMajor(lvpCol, p);
        CHECK(s.rawDepth >= 0.0f);
        CHECK(s.rawDepth <= 1.0f);
        CHECK(s.refDepth >= 0.0f);
        CHECK(s.refDepth <= 1.0f);
        CHECK(s.uvIn01);
    }
}

TEST_CASE(scene_bounds_use_real_mesh_extent_and_ignore_overlay_domain)
{
    ayt::render::detail::GpuMesh chunkMesh;
    chunkMesh.localBoundsValid = true;
    chunkMesh.localBoundsMin = FVector3(0.0f, 0.0f, 0.0f);
    chunkMesh.localBoundsMax = FVector3(32.0f, 64.0f, 0.0f);
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    meshes.emplace(1u, chunkMesh);

    ayt::render::DrawPayload2D worldPayload;
    worldPayload.renderDomain = ayt::render::RenderDomain2D::WorldLit;
    ayt::render::DrawItem worldItem;
    worldItem.mesh.id = 1u;
    worldItem.payload = &worldPayload;
    worldItem.world.row[0].w = 10.0f;
    worldItem.world.row[1].w = 20.0f;
    worldItem.world.row[2].w = 30.0f;

    ayt::render::DrawPayload2D overlayPayload;
    ayt::render::DrawItem overlayItem = worldItem;
    overlayItem.payload = &overlayPayload;
    overlayItem.world.row[0].w = 10000.0f;

    ayt::render::DrawItem disabledItem = worldItem;
    disabledItem.payload = nullptr;
    disabledItem.shadowFlags = ayt::render::ShadowFlags::None;
    disabledItem.world.row[1].w = -10000.0f;

    ayt::render::RenderScene scene;
    scene.add(overlayItem);
    scene.add(disabledItem);
    scene.add(worldItem);

    const ShadowSceneBounds bounds = computeShadowSceneBounds(scene, meshes);
    CHECK(bounds.valid);
    CHECK_FLOAT_EQ(bounds.min.x, 10.0f, 1e-6f);
    CHECK_FLOAT_EQ(bounds.min.y, 20.0f, 1e-6f);
    CHECK_FLOAT_EQ(bounds.min.z, 30.0f, 1e-6f);
    CHECK_FLOAT_EQ(bounds.max.x, 42.0f, 1e-6f);
    CHECK_FLOAT_EQ(bounds.max.y, 84.0f, 1e-6f);
    CHECK_FLOAT_EQ(bounds.max.z, 30.0f, 1e-6f);
}

TEST_SUITE_END
