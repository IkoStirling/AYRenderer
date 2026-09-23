#include "detail/ShadowMatrixBuilder.h"

#include "detail/BgfxMatrix.h"
#include "detail/Draw2D.h"

#include "AYRenderer/ShadowConfig.h"

#include "AYMath/MathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace ayt::render::detail
{

namespace {

constexpr float kBoundsMargin = 2.0f;
constexpr float kMinOrthoRadius = 6.0f;
constexpr float kMinZSpan = 8.0f; // was ±24 forced span → depths ~1.0 → cleared kills shadows
constexpr float kPi = 3.14159265358979323846f;

ayt::math::FVector3 normalizeLightDir(const ayt::math::FVector3& lightDirection)
{
    ayt::math::FVector3 dir = lightDirection;
    const float lenSq = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    if (lenSq < 1.0e-12f) {
        dir = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    }
    return dir.normalize();
}

void expandBoundsPoint(ShadowSceneBounds& bounds, const ayt::math::FVector3& p)
{
    if (!bounds.valid) {
        bounds.min     = p;
        bounds.max     = p;
        bounds.center  = p;
        bounds.valid   = true;
        return;
    }
    bounds.min.x = std::fmin(bounds.min.x, p.x);
    bounds.min.y = std::fmin(bounds.min.y, p.y);
    bounds.min.z = std::fmin(bounds.min.z, p.z);
    bounds.max.x = std::fmax(bounds.max.x, p.x);
    bounds.max.y = std::fmax(bounds.max.y, p.y);
    bounds.max.z = std::fmax(bounds.max.z, p.z);
    bounds.center = ayt::math::FVector3(
        (bounds.min.x + bounds.max.x) * 0.5f,
        (bounds.min.y + bounds.max.y) * 0.5f,
        (bounds.min.z + bounds.max.z) * 0.5f);
}

void expandItemBounds(ShadowSceneBounds& bounds,
                      const DrawItem& item,
                      const std::unordered_map<uint64_t, GpuMesh>& meshes)
{
    if (!item.mesh.isValid() || !isShadowDomainItem(item)
        || (!castsShadow(item.shadowFlags)
            && !receivesShadow(item.shadowFlags))) {
        return;
    }
    const auto meshIt = meshes.find(item.mesh.id);
    if (meshIt == meshes.end()) {
        return;
    }

    const GpuMesh& mesh = meshIt->second;
    const ayt::math::FVector3 localMin = mesh.localBoundsValid
        ? mesh.localBoundsMin : ayt::math::FVector3(-0.5f, -0.5f, -0.5f);
    const ayt::math::FVector3 localMax = mesh.localBoundsValid
        ? mesh.localBoundsMax : ayt::math::FVector3(0.5f, 0.5f, 0.5f);

    // Transform all local AABB corners. Flat Sprite/Tilemap meshes naturally
    // have min.z == max.z; duplicate corners are harmless and keep one path.
    const float* m = item.world.ptr();
    for (int i = 0; i < 8; ++i) {
        const float lx = (i & 1) ? localMax.x : localMin.x;
        const float ly = (i & 2) ? localMax.y : localMin.y;
        const float lz = (i & 4) ? localMax.z : localMin.z;
        const float wx = m[0] * lx + m[1] * ly + m[2] * lz + m[3];
        const float wy = m[4] * lx + m[5] * ly + m[6] * lz + m[7];
        const float wz = m[8] * lx + m[9] * ly + m[10] * lz + m[11];
        expandBoundsPoint(bounds, ayt::math::FVector3(wx, wy, wz));
    }
}

// §P4 L11 (2026-08-24) — documented the LH-ortho contract for
// the default editor/play bounds. The bounds here are
// hand-tuned for a 10×2.5×10 box centered at (0, 0.5, 0) in
// AYMath left-handed world space (camera looks down +Z;
// +Y is up; +X is right). The downstream lh::lookAt +
// lh::ortho (LH convention) reads these bounds as
// axis-aligned corners — see `buildFromBoundsInternal` for
// the LH/Z+ view+proj setup. Don't swap these to a right-
// handed (RH) value set without also migrating
// `buildDirectionalShadowMatricesForScene` to the rh::
// namespace (see cutsheet "保持左手系" / "将手性坐标移入
// rh 命名空间下").
ShadowSceneBounds defaultEditorPlayBounds()
{
    ShadowSceneBounds bounds{};
    bounds.valid  = true;
    bounds.min    = ayt::math::FVector3(-5.0f, -0.5f, -5.0f);
    bounds.max    = ayt::math::FVector3(5.0f, 2.0f, 5.0f);
    bounds.center = ayt::math::FVector3(0.0f, 0.5f, 0.0f);
    return bounds;
}

void buildFromBoundsInternal(
    const ShadowSceneBounds& boundsIn,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16])
{
    ShadowSceneBounds bounds = boundsIn.valid ? boundsIn : defaultEditorPlayBounds();

    const ayt::math::FVector3 dir = normalizeLightDir(lightDirection);
    const ayt::math::FVector3 center = bounds.center;

    const float extentX = (bounds.max.x - bounds.min.x) * 0.5f;
    const float extentY = (bounds.max.y - bounds.min.y) * 0.5f;
    const float extentZ = (bounds.max.z - bounds.min.z) * 0.5f;
    const float radius  = std::fmax(kMinOrthoRadius,
                                    std::sqrt(extentX * extentX + extentY * extentY
                                              + extentZ * extentZ) + kBoundsMargin);

    const ayt::math::FVector3 eye(
        center.x - dir.x * radius * 2.0f,
        center.y - dir.y * radius * 2.0f,
        center.z - dir.z * radius * 2.0f);

    ayt::math::FVector3 up(0.0f, 1.0f, 0.0f);
    const float upDot = dir.x * up.x + dir.y * up.y + dir.z * up.z;
    if (std::fabs(upDot) > 0.99f) {
        up = ayt::math::FVector3(0.0f, 0.0f, 1.0f);
    }

    // Engine convention is LH (see AYMath/MathUtils.h lh::). lh::lookAt
    // returns a row-major Float4x4 — same numerical basis vectors as the
    // previous bx::mtxLookAt (column-major) since storage layout is just
    // transposed.
    outView = ayt::math::lh::lookAt(eye, center, up);

    // Project world AABB corners into light view, then use a STABLE SQUARE
    // ortho (max half-extent). Tight non-square fit + per-frame rotation
    // caused an "invisible square" clip and holes when UVs left the map.
    float minX =  1.0e9f;
    float maxX = -1.0e9f;
    float minY =  1.0e9f;
    float maxY = -1.0e9f;
    float minZ =  1.0e9f;
    float maxZ = -1.0e9f;

    const float xs[2] = {bounds.min.x, bounds.max.x};
    const float ys[2] = {bounds.min.y, bounds.max.y};
    const float zs[2] = {bounds.min.z, bounds.max.z};
    for (float x : xs) {
        for (float y : ys) {
            for (float z : zs) {
                const ayt::math::FVector4 viewH = outView
                    * ayt::math::FVector4(x, y, z, 1.0f);
                minX = std::fmin(minX, viewH.x);
                maxX = std::fmax(maxX, viewH.x);
                minY = std::fmin(minY, viewH.y);
                maxY = std::fmax(maxY, viewH.y);
                minZ = std::fmin(minZ, viewH.z);
                maxZ = std::fmax(maxZ, viewH.z);
            }
        }
    }

    const float midX = 0.5f * (minX + maxX);
    const float midY = 0.5f * (minY + maxY);
    const float halfX = 0.5f * (maxX - minX);
    const float halfY = 0.5f * (maxY - minY);
    const float orthoR = std::fmax(kMinOrthoRadius,
                                   std::fmax(halfX, halfY) + kBoundsMargin);
    minX = midX - orthoR;
    maxX = midX + orthoR;
    minY = midY - orthoR;
    maxY = midY + orthoR;

    // Tight Z around the light-view AABB (with a modest minimum span).
    // A huge forced ±zExtent collapses encoded depth toward 1.0 so the
    // receiver's cleared-texel path marks everything fully lit.
    float z0 = minZ - kBoundsMargin;
    float z1 = maxZ + kBoundsMargin;
    if (z1 < z0) {
        std::swap(z0, z1);
    }
    if ((z1 - z0) < kMinZSpan) {
        const float midZ = 0.5f * (z0 + z1);
        z0 = midZ - 0.5f * kMinZSpan;
        z1 = midZ + 0.5f * kMinZSpan;
    }

    outProj = ayt::math::lh::ortho(minX, maxX,
                                   minY, maxY,
                                   z0, z1);

    toBgfxColumnMajor(outView, outViewColMajor);
    toBgfxColumnMajor(outProj, outProjColMajor);
    // bgfx setViewTransform(view, proj) → clip = P * V * M. AYMath uses
    // operator* with the standard convention: (P * V) * M == P * (V * M).
    const ayt::math::Float4x4 viewProj = outProj * outView;
    toBgfxColumnMajor(viewProj, outViewProjColMajor);
    outViewProj = viewProj;
}

void writeMatrices(const ayt::math::Float4x4& view,
                   const ayt::math::Float4x4& projection,
                   ayt::math::Float4x4& outView,
                   ayt::math::Float4x4& outProj,
                   ayt::math::Float4x4& outViewProj,
                   float outViewColMajor[16],
                   float outProjColMajor[16],
                   float outViewProjColMajor[16])
{
    outView = view;
    outProj = projection;
    outViewProj = projection * view;
    toBgfxColumnMajor(outView, outViewColMajor);
    toBgfxColumnMajor(outProj, outProjColMajor);
    toBgfxColumnMajor(outViewProj, outViewProjColMajor);
}

bool perspectiveRange(const ayt::math::Float4x4& projection,
                      float& nearZ,
                      float& farZ) noexcept
{
    const float m22 = projection(2, 2);
    const float m23 = projection(2, 3);
    const bool perspective = std::fabs(projection(3, 2)) > 0.5f
        && std::fabs(projection(3, 3)) < 1.0e-5f
        && std::isfinite(m22) && std::isfinite(m23);
    if (!perspective || std::fabs(m22) < 1.0e-6f
        || std::fabs(m22 - 1.0f) < 1.0e-6f) {
        return false;
    }
    nearZ = -m23 / m22;
    farZ = -m23 / (m22 - 1.0f);
    return std::isfinite(nearZ) && std::isfinite(farZ)
        && nearZ > 0.0f && farZ > nearZ;
}

ayt::math::FVector3 homogenizedPoint(const ayt::math::FVector4& p)
{
    const float safeW = std::fabs(p.w) > 1.0e-6f ? p.w : 1.0f;
    return ayt::math::FVector3(p.x / safeW, p.y / safeW, p.z / safeW);
}

void includeBoundsDepth(const ShadowSceneBounds& bounds,
                        const ayt::math::Float4x4& lightView,
                        float& minZ,
                        float& maxZ)
{
    if (!bounds.valid) {
        return;
    }
    for (uint32_t corner = 0; corner < 8u; ++corner) {
        const ayt::math::FVector4 p(
            (corner & 1u) ? bounds.max.x : bounds.min.x,
            (corner & 2u) ? bounds.max.y : bounds.min.y,
            (corner & 4u) ? bounds.max.z : bounds.min.z,
            1.0f);
        const ayt::math::FVector4 lightP = lightView * p;
        minZ = std::min(minZ, lightP.z);
        maxZ = std::max(maxZ, lightP.z);
    }
}

} // namespace

ShadowSceneBounds computeShadowSceneBounds(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes)
{
    std::vector<const DrawItem*> items;
    items.reserve(scene.items().size());
    for (const DrawItem& item : scene.items()) {
        items.push_back(&item);
    }
    return computeShadowSceneBounds(items, meshes);
}

ShadowSceneBounds computeShadowSceneBounds(
    const std::vector<const DrawItem*>& items,
    const std::unordered_map<uint64_t, GpuMesh>& meshes)
{
    ShadowSceneBounds bounds{};
    for (const DrawItem* item : items) {
        if (item != nullptr) {
            expandItemBounds(bounds, *item, meshes);
        }
    }
    return bounds;
}

void buildDirectionalShadowMatricesForScene(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16])
{
    ShadowSceneBounds bounds = computeShadowSceneBounds(scene, meshes);
    if (!bounds.valid) {
        bounds = defaultEditorPlayBounds();
    }
    buildFromBoundsInternal(bounds,
                            lightDirection,
                            outView,
                            outProj,
                            outViewProj,
                            outViewColMajor,
                            outProjColMajor,
                            outViewProjColMajor);
}

void buildDirectionalShadowMatricesForItems(
    const std::vector<const DrawItem*>& items,
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16])
{
    ShadowSceneBounds bounds = computeShadowSceneBounds(items, meshes);
    if (!bounds.valid) {
        bounds = defaultEditorPlayBounds();
    }
    buildFromBoundsInternal(bounds,
                            lightDirection,
                            outView,
                            outProj,
                            outViewProj,
                            outViewColMajor,
                            outProjColMajor,
                            outViewProjColMajor);
}

void buildDirectionalShadowMatricesFromBounds(
    const ShadowSceneBounds& bounds,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16])
{
    buildFromBoundsInternal(bounds,
                            lightDirection,
                            outView,
                            outProj,
                            outViewProj,
                            outViewColMajor,
                            outProjColMajor,
                            outViewProjColMajor);
}

DirectionalCascadeSplits computeDirectionalCascadeSplits(
    const ayt::math::Float4x4& cameraProjection,
    uint32_t cascadeCount,
    float lambda,
    float maxDistance) noexcept
{
    DirectionalCascadeSplits result{};
    cascadeCount = std::clamp(cascadeCount, 1u, 3u);
    lambda = std::clamp(std::isfinite(lambda) ? lambda : 0.65f, 0.0f, 1.0f);
    maxDistance = std::max(
        std::isfinite(maxDistance) ? maxDistance : 200.0f, 1.0f);

    float nearZ = 0.1f;
    float farZ = maxDistance;
    if (!perspectiveRange(cameraProjection, nearZ, farZ)) {
        return result;
    }
    farZ = std::min(farZ, std::max(maxDistance, nearZ + 1.0f));
    for (uint32_t i = 1; i <= cascadeCount; ++i) {
        const float ratio = static_cast<float>(i)
            / static_cast<float>(cascadeCount);
        const float logarithmic = nearZ * std::pow(farZ / nearZ, ratio);
        const float uniform = nearZ + (farZ - nearZ) * ratio;
        result.distances[i - 1u] =
            logarithmic * lambda + uniform * (1.0f - lambda);
    }
    result.count = cascadeCount;
    return result;
}

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
    float outViewProjColMajor[16])
{
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    if (!perspectiveRange(cameraProjection, cameraNear, cameraFar)
        || !std::isfinite(sliceNear) || !std::isfinite(sliceFar)) {
        return false;
    }
    sliceNear = std::max(sliceNear, cameraNear);
    sliceFar = std::min(sliceFar, cameraFar);
    if (sliceFar <= sliceNear) {
        return false;
    }

    const float invProjX = 1.0f / std::max(
        std::fabs(cameraProjection(0, 0)), 1.0e-6f);
    const float invProjY = 1.0f / std::max(
        std::fabs(cameraProjection(1, 1)), 1.0e-6f);
    const ayt::math::Float4x4 inverseView = cameraView.inverse();
    ayt::math::FVector3 corners[8];
    ayt::math::FVector3 center(0.0f, 0.0f, 0.0f);
    uint32_t index = 0;
    for (uint32_t zIndex = 0; zIndex < 2u; ++zIndex) {
        const float z = zIndex == 0u ? sliceNear : sliceFar;
        for (uint32_t yIndex = 0; yIndex < 2u; ++yIndex) {
            for (uint32_t xIndex = 0; xIndex < 2u; ++xIndex) {
                const float xSign = xIndex == 0u ? -1.0f : 1.0f;
                const float ySign = yIndex == 0u ? -1.0f : 1.0f;
                corners[index] = homogenizedPoint(inverseView * ayt::math::FVector4(
                    xSign * z * invProjX,
                    ySign * z * invProjY,
                    z,
                    1.0f));
                center += corners[index];
                ++index;
            }
        }
    }
    center /= 8.0f;

    float radius = 0.0f;
    for (const ayt::math::FVector3& corner : corners) {
        radius = std::max(radius, (corner - center).length());
    }
    radius = std::ceil(std::max(radius, 1.0f) * 16.0f) / 16.0f;

    const ayt::math::FVector3 dir = normalizeLightDir(lightDirection);
    ayt::math::FVector3 up(0.0f, 1.0f, 0.0f);
    if (std::fabs(dir.dot(up)) > 0.99f) {
        up = ayt::math::FVector3(0.0f, 0.0f, 1.0f);
    }
    const ayt::math::FVector3 eye = center - dir * (radius * 2.0f);
    const ayt::math::Float4x4 lightView = ayt::math::lh::lookAt(eye, center, up);

    const ayt::math::FVector4 lightCenter = lightView
        * ayt::math::FVector4(center.x, center.y, center.z, 1.0f);
    const float resolution = static_cast<float>(std::max<uint16_t>(
        shadowResolution, 1u));
    const float worldUnitsPerTexel = (radius * 2.0f) / resolution;
    const float snappedX = std::floor(lightCenter.x / worldUnitsPerTexel + 0.5f)
        * worldUnitsPerTexel;
    const float snappedY = std::floor(lightCenter.y / worldUnitsPerTexel + 0.5f)
        * worldUnitsPerTexel;

    float minZ = 1.0e9f;
    float maxZ = -1.0e9f;
    for (const ayt::math::FVector3& corner : corners) {
        const ayt::math::FVector4 p = lightView
            * ayt::math::FVector4(corner.x, corner.y, corner.z, 1.0f);
        minZ = std::min(minZ, p.z);
        maxZ = std::max(maxZ, p.z);
    }
    includeBoundsDepth(sceneBounds, lightView, minZ, maxZ);
    minZ -= kBoundsMargin;
    maxZ += kBoundsMargin;
    if ((maxZ - minZ) < kMinZSpan) {
        const float midZ = (minZ + maxZ) * 0.5f;
        minZ = midZ - kMinZSpan * 0.5f;
        maxZ = midZ + kMinZSpan * 0.5f;
    }

    const ayt::math::Float4x4 lightProjection = ayt::math::lh::ortho(
        snappedX - radius, snappedX + radius,
        snappedY - radius, snappedY + radius,
        minZ, maxZ);
    writeMatrices(lightView, lightProjection,
                  outView, outProj, outViewProj,
                  outViewColMajor, outProjColMajor,
                  outViewProjColMajor);
    return true;
}

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
    float outViewProjColMajor[16])
{
    const ayt::math::FVector3 dir = normalizeLightDir(direction);
    ayt::math::FVector3 up(0.0f, 1.0f, 0.0f);
    if (std::fabs(dir.dot(up)) > 0.99f) {
        up = ayt::math::FVector3(0.0f, 0.0f, 1.0f);
    }
    const float safeCos = std::clamp(
        std::isfinite(coneCosOuter) ? coneCosOuter : 0.5f,
        -0.996f, 0.9999f);
    const float fov = std::clamp(2.0f * std::acos(safeCos),
                                 kPi / 180.0f,
                                 kPi * (175.0f / 180.0f));
    const float farZ = std::max(
        std::isfinite(range) ? range : 1.0f, 0.1f);
    const float nearZ = std::min(std::max(farZ * 0.01f, 0.025f), farZ * 0.5f);
    const ayt::math::Float4x4 view = ayt::math::lh::lookAt(
        position, position + dir, up);
    const ayt::math::Float4x4 projection = ayt::math::lh::perspective(
        fov, 1.0f, nearZ, farZ);
    writeMatrices(view, projection,
                  outView, outProj, outViewProj,
                  outViewColMajor, outProjColMajor,
                  outViewProjColMajor);
}

} // namespace ayt::render::detail
