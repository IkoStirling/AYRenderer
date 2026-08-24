#include "detail/ShadowLightMatrix.h"
#include "detail/BgfxMatrix.h"

#include "AYRenderer/ShadowConfig.h"

#include "AYMath/MathUtils.h"

#include <cmath>
#include <cstring>

namespace ayt::render::detail
{

void buildDirectionalShadowMatrices(
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16],
    ayt::math::FVector3 focus,
    float radius,
    bool /*homogeneousDepth*/)
{
    if (radius <= 0.0f) {
        radius = 50.0f;
    }

    ayt::math::FVector3 dir = lightDirection;
    const float lenSq = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    if (lenSq < 1.0e-12f) {
        dir = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    }
    dir = dir.normalize();

    const ayt::math::FVector3 eye(
        focus.x - dir.x * radius,
        focus.y - dir.y * radius,
        focus.z - dir.z * radius);

    ayt::math::FVector3 up(0.0f, 1.0f, 0.0f);
    const float upDot = dir.x * up.x + dir.y * up.y + dir.z * up.z;
    if (std::fabs(upDot) > 0.99f) {
        up = ayt::math::FVector3(0.0f, 0.0f, 1.0f);
    }

    // Engine convention is LH (see AYMath/MathUtils.h lh::). Shadow maps
    // therefore use [0,1] depth clip space (D3D-style homogeneousDepth).
    // The legacy `homogeneousDepth` parameter is kept for ABI symmetry;
    // bgfx::getCaps()->homogeneousDepth == true is implicit.
    outView = ayt::math::lh::lookAt(eye, focus, up);
    outProj = ayt::math::lh::ortho(-radius, radius,
                                   -radius, radius,
                                   ayt::render::kShadowNearPlane,
                                   ayt::render::kShadowFarPlane);

    toBgfxColumnMajor(outView, outViewColMajor);
    toBgfxColumnMajor(outProj, outProjColMajor);
    // bgfx setViewTransform(view, proj) → clip = P * V * M. AYMath uses
    // operator* with the standard convention: (P * V) * M == P * (V * M).
    const ayt::math::Float4x4 viewProj = outProj * outView;
    toBgfxColumnMajor(viewProj, outViewProjColMajor);
    outViewProj = viewProj;
}

} // namespace ayt::render::detail
