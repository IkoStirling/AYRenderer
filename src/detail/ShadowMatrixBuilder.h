#pragma once

#include "AYRenderScene.h"
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

// Merge axis-aligned bounds from draw items (centered unit mesh * world scale).
ShadowSceneBounds computeShadowSceneBounds(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes);

// Scene-fitted directional shadow matrices (bgfx example 16 ortho Z convention).
void buildDirectionalShadowMatricesForScene(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16],
    bool homogeneousDepth);

// Test / fallback entry when bounds are supplied directly.
void buildDirectionalShadowMatricesFromBounds(
    const ShadowSceneBounds& bounds,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16],
    bool homogeneousDepth);

} // namespace ayt::render::detail
