#include "detail/BgfxMatrix.h"

#include "aymath/MathUtils.h"

#include "AYTest.h"

#include <cmath>

using ayt::math::Float4x4;
using ayt::math::FVector3;
using ayt::math::translate;
using ayt::render::detail::fromBgfxColumnMajor;
using ayt::render::detail::toBgfxColumnMajor;

namespace {

bool nearlyEqual(float a, float b, float eps = 1.0e-5f)
{
    return std::fabs(a - b) <= eps;
}

bool matricesNearlyEqual(const Float4x4& a, const Float4x4& b, float eps = 1.0e-5f)
{
    const float* pa = a.ptr();
    const float* pb = b.ptr();
    for (int i = 0; i < 16; ++i) {
        if (!nearlyEqual(pa[i], pb[i], eps)) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_SUITE(BgfxMatrixTests)

TEST_CASE(bgfx_matrix_translate_y_lands_in_m13)
{
    const Float4x4 world = translate(FVector3(0.0f, 0.5f, 0.0f));
    float col[16] = {};
    toBgfxColumnMajor(world, col);

    CHECK(nearlyEqual(col[12], 0.0f));
    CHECK(nearlyEqual(col[13], 0.5f));
    CHECK(nearlyEqual(col[14], 0.0f));
    CHECK(nearlyEqual(col[15], 1.0f));
    // Identity rotation block.
    CHECK(nearlyEqual(col[0], 1.0f));
    CHECK(nearlyEqual(col[5], 1.0f));
    CHECK(nearlyEqual(col[10], 1.0f));
}

TEST_CASE(bgfx_matrix_round_trip_preserves_aymath)
{
    const Float4x4 original = translate(FVector3(1.25f, -0.5f, 3.0f));
    float col[16] = {};
    toBgfxColumnMajor(original, col);
    const Float4x4 restored = fromBgfxColumnMajor(col);
    CHECK(matricesNearlyEqual(original, restored));
}

TEST_CASE(bgfx_matrix_aymath_translation_in_row_w)
{
    const Float4x4 world = translate(FVector3(2.0f, 4.0f, 6.0f));
    CHECK(nearlyEqual(world.row[0].w, 2.0f));
    CHECK(nearlyEqual(world.row[1].w, 4.0f));
    CHECK(nearlyEqual(world.row[2].w, 6.0f));
}

TEST_CASE(bgfx_matrix_editor_cube_height_maps_to_m13)
{
    // Editor places the cube at y=1.25; confirm upload packing.
    const Float4x4 world = translate(FVector3(0.0f, 1.25f, 0.0f));
    float col[16] = {};
    toBgfxColumnMajor(world, col);
    CHECK(nearlyEqual(col[12], 0.0f));
    CHECK(nearlyEqual(col[13], 1.25f));
    CHECK(nearlyEqual(col[14], 0.0f));
}

TEST_SUITE_END
