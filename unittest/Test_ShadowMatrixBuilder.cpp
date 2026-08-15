#include "detail/ShadowDebug.h"
#include "detail/ShadowMatrixBuilder.h"

#include "AYMath/MathTypes.h"

#include "AYTest.h"

#include <cmath>

using ayt::math::Float4x4;
using ayt::math::FVector3;
using ayt::render::detail::ShadowSceneBounds;
using ayt::render::detail::ShadowProjectSample;
using ayt::render::detail::buildDirectionalShadowMatricesFromBounds;
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
        lvpCol,
        /*homogeneousDepth=*/false);

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
        lvpCol,
        false);

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

TEST_SUITE_END
