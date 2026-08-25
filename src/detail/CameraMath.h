#pragma once

#include "AYMath/MathUtils.h"

namespace ayt::render::detail
{

// Renderer camera APIs retain the bx-era public contract: vertical FOV is
// expressed in degrees. AYMath's projection helpers use radians.
inline ayt::math::Float4x4 makeLeftHandedPerspectiveDegrees(
    float fovYDegrees,
    float aspect,
    float nearZ,
    float farZ)
{
    return ayt::math::lh::perspective(
        ayt::math::radians(fovYDegrees), aspect, nearZ, farZ);
}

} // namespace ayt::render::detail
