#pragma once

#include <AYResource/GeometryConvention.h>
#include <AYMath/MathTypes.h>
#include <bgfx/bgfx.h>

namespace ayt::render::detail
{

// AY engine assets are left-handed (+Z forward) with outward-facing triangle
// normals. Through the LH view/projection path those front faces have clockwise
// clip-space winding, so normal rendering culls counter-clockwise back faces.
// Keep the two states named here: spelling BGFX_STATE_CULL_* directly in a pass
// previously let Forward/GBuffer disagree with the imported mesh convention.
inline constexpr uint64_t kCullBackFaces  = BGFX_STATE_CULL_CCW;
inline constexpr uint64_t kCullFrontFaces = BGFX_STATE_CULL_CW;

static_assert(ayt::resource::kMeshCoordinatesAreLeftHanded);
static_assert(ayt::resource::kMeshFrontFaceWinding
              == ayt::resource::MeshFrontFaceWinding::Clockwise);

inline float linearDeterminant(const ayt::math::Float4x4& transform) noexcept
{
    const float* m = transform.ptr();
    return m[0] * (m[5] * m[10] - m[6] * m[9])
         - m[1] * (m[4] * m[10] - m[6] * m[8])
         + m[2] * (m[4] * m[9]  - m[5] * m[8]);
}

inline bool reversesWinding(const ayt::math::Float4x4& transform) noexcept
{
    return linearDeterminant(transform) < 0.0f;
}

// Normals are covectors: a non-uniform model scale must use inverse-transpose
// instead of the position/tangent linear transform. Singular transforms have
// no mathematically defined normal matrix; identity is a stable fallback for
// zero-scale editor states and avoids feeding infinities to the shader.
inline ayt::math::Float4x4 normalMatrixForTransform(
    const ayt::math::Float4x4& transform) noexcept
{
    const float determinant = linearDeterminant(transform);
    if (determinant > -1.0e-8f && determinant < 1.0e-8f) {
        return ayt::math::Float4x4::identity();
    }
    return transform.inverse_fast().transpose();
}

inline uint64_t cullBackFacesForTransform(
    const ayt::math::Float4x4& transform) noexcept
{
    return reversesWinding(transform) ? kCullFrontFaces : kCullBackFaces;
}

inline uint64_t cullFrontFacesForTransform(
    const ayt::math::Float4x4& transform) noexcept
{
    return reversesWinding(transform) ? kCullBackFaces : kCullFrontFaces;
}

} // namespace ayt::render::detail
