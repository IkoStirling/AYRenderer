#include "detail/ShadowDebug.h"
#include "detail/ShadowMatrixBuilder.h"

#include "AYRenderer/RenderScene.h"
#include "AYMath/MathTypes.h"
#include "AYMath/MathUtils.h"

#include "AYTest.h"

#include <cmath>
#include <unordered_map>

using ayt::math::Float4x4;
using ayt::math::FVector3;
using ayt::render::detail::ShadowSceneBounds;
using ayt::render::detail::ShadowProjectSample;
using ayt::render::detail::buildDirectionalShadowMatricesFromBounds;
using ayt::render::detail::buildDirectionalCascadeMatrices;
using ayt::render::detail::buildSpotShadowMatrices;
using ayt::render::detail::computeDirectionalCascadeSplits;
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

TEST_CASE(cascade_splits_are_practical_monotonic_camera_distances)
{
    const Float4x4 projection = ayt::math::lh::perspective(
        ayt::math::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const auto splits = computeDirectionalCascadeSplits(
        projection, 3u, 0.65f, 200.0f);
    CHECK(splits.count == 3u);
    CHECK(splits.distances[0] > 0.1f);
    CHECK(splits.distances[1] > splits.distances[0]);
    CHECK(splits.distances[2] > splits.distances[1]);
    CHECK(splits.distances[2] <= 200.01f);
}

TEST_CASE(camera_slice_directional_matrix_covers_slice_center)
{
    const Float4x4 cameraView = Float4x4::identity();
    const Float4x4 cameraProjection = ayt::math::lh::perspective(
        ayt::math::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    Float4x4 view{};
    Float4x4 projection{};
    Float4x4 viewProjection{};
    float viewCol[16]{};
    float projectionCol[16]{};
    float viewProjectionCol[16]{};
    const bool built = buildDirectionalCascadeMatrices(
        cameraView, cameraProjection, 0.1f, 20.0f, ShadowSceneBounds{},
        FVector3(0.35f, -0.85f, -0.4f), 1024u,
        view, projection, viewProjection,
        viewCol, projectionCol, viewProjectionCol);
    CHECK(built);
    const ShadowProjectSample center = projectWorldThroughLvpColMajor(
        viewProjectionCol, FVector3(0.0f, 0.0f, 10.0f));
    CHECK(center.uvIn01);
    CHECK(center.rawDepth >= 0.0f);
    CHECK(center.rawDepth <= 1.0f);
}

TEST_CASE(spot_shadow_uses_position_cone_and_perspective_projection)
{
    Float4x4 view{};
    Float4x4 projection{};
    Float4x4 viewProjection{};
    float viewCol[16]{};
    float projectionCol[16]{};
    float viewProjectionCol[16]{};
    buildSpotShadowMatrices(
        FVector3(2.0f, 3.0f, -4.0f), FVector3(0.0f, 0.0f, 1.0f),
        std::cos(ayt::math::radians(30.0f)), 25.0f,
        view, projection, viewProjection,
        viewCol, projectionCol, viewProjectionCol);
    CHECK(std::fabs(projection(3, 2) - 1.0f) < 1.0e-5f);
    CHECK(std::fabs(projection(3, 3)) < 1.0e-5f);
    const ShadowProjectSample axis = projectWorldThroughLvpColMajor(
        viewProjectionCol, FVector3(2.0f, 3.0f, 6.0f));
    CHECK(axis.uvIn01);
    CHECK(nearlyEqual(axis.shadowU, 0.5f, 0.01f));
    CHECK(nearlyEqual(axis.shadowV, 0.5f, 0.01f));
}

TEST_SUITE_END
