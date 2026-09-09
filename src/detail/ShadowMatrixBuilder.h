#pragma once

#include "AYRenderer/RenderScene.h"
#include "detail/GpuResources.h"

#include "AYMath/MathTypes.h"

#include <unordered_map>

namespace ayt::render::detail
{

struct ShadowSceneBounds {
    ayt::math::FVector3 min = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
    ayt::math::FVector3 max = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
    ayt::math::FVector3 center = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
    bool valid = false;
};

// Merge axis-aligned bounds from world-domain shadow participants using each
// uploaded mesh's bind-pose local bounds. Overlay2D and ShadowFlags::None do
// not enlarge the fitted light frustum.
ShadowSceneBounds computeShadowSceneBounds(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes);

// Scene-fitted directional shadow matrices (AYMath lh:: — engine LH
// convention, [0,1] depth clip space).
void buildDirectionalShadowMatricesForScene(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16]);

// Test / fallback entry when bounds are supplied directly.
void buildDirectionalShadowMatricesFromBounds(
    const ShadowSceneBounds& bounds,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16]);

} // namespace ayt::render::detail
