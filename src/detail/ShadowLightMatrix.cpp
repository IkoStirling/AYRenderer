#include "detail/ShadowLightMatrix.h"

#include <bx/math.h>

#include <cmath>
#include <cstring>

namespace ayt::render::detail
{

void buildDirectionalShadowMatrices(
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::FVector3 focus,
    float radius,
    bool homogeneousDepth)
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

    // Eye sits opposite the travel direction so the light looks toward focus.
    const ayt::math::FVector3 eye(
        focus.x - dir.x * radius,
        focus.y - dir.y * radius,
        focus.z - dir.z * radius);

    ayt::math::FVector3 up(0.0f, 1.0f, 0.0f);
    const float upDot = dir.x * up.x + dir.y * up.y + dir.z * up.z;
    if (std::fabs(upDot) > 0.99f) {
        up = ayt::math::FVector3(0.0f, 0.0f, 1.0f);
    }

    float viewBx[16];
    float projBx[16];
    bx::mtxLookAt(viewBx,
                  bx::Vec3{eye.x, eye.y, eye.z},
                  bx::Vec3{focus.x, focus.y, focus.z},
                  bx::Vec3{up.x, up.y, up.z});
    bx::mtxOrtho(projBx,
                 -radius, radius,
                 -radius, radius,
                 0.1f, radius * 2.0f,
                 0.0f,
                 homogeneousDepth);

    std::memcpy(outView.ptr(), viewBx, sizeof(viewBx));
    std::memcpy(outProj.ptr(), projBx, sizeof(projBx));
}

} // namespace ayt::render::detail
