#pragma once

#include "AYRenderer/RenderScene.h"
#include "detail/GpuResources.h"

#include "AYMath/MathTypes.h"

#include <unordered_map>
#include <vector>

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
ShadowSceneBounds computeShadowSceneBounds(
    const std::vector<const DrawItem*>& items,
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
void buildDirectionalShadowMatricesForItems(
    const std::vector<const DrawItem*>& items,
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

// Practical split scheme for the camera-facing directional cascades. The
// returned distances are positive camera-view Z distances. The last split is
// capped so an extremely large gameplay far plane cannot destroy near-shadow
// texel density; the legacy scene-fit projection remains the far fallback.
struct DirectionalCascadeSplits {
    float distances[3]{};
    uint32_t count = 0;
};

DirectionalCascadeSplits computeDirectionalCascadeSplits(
    const ayt::math::Float4x4& cameraProjection,
    uint32_t cascadeCount = 3u,
    float lambda = 0.65f,
    float maxDistance = 200.0f) noexcept;

// Fit one stabilized orthographic projection to a camera frustum slice.
// sceneBounds contributes only caster Z range; XY remains camera-slice fitted
// so off-screen casters do not dilute visible texel density.
bool buildDirectionalCascadeMatrices(
    const ayt::math::Float4x4& cameraView,
    const ayt::math::Float4x4& cameraProjection,
    float sliceNear,
    float sliceFar,
    const ShadowSceneBounds& sceneBounds,
    const ayt::math::FVector3& lightDirection,
    uint16_t shadowResolution,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16]);

// Spot lights use their real position, direction, outer cone and range. This
// is intentionally independent of scene bounds: the light cone is the
// authoritative receiver/caster volume.
void buildSpotShadowMatrices(
    const ayt::math::FVector3& position,
    const ayt::math::FVector3& direction,
    float coneCosOuter,
    float range,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16]);

} // namespace ayt::render::detail
