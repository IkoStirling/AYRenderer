#include "detail/ShadowMatrixBuilder.h"

#include "detail/BgfxMatrix.h"

#include "AYRenderer/ShadowConfig.h"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ayt::render::detail
{

namespace {

constexpr float kBoundsMargin = 2.0f;
constexpr float kMinOrthoRadius = 6.0f;
constexpr float kMinZSpan = 8.0f; // was ±24 forced span → depths ~1.0 → cleared kills shadows

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
    if (!item.mesh.isValid()) {
        return;
    }
    const auto meshIt = meshes.find(item.mesh.id);
    if (meshIt == meshes.end()) {
        return;
    }

    // Unit-cube local AABB [-0.5,0.5]^3 → 8 corners through world matrix.
    // (Previous center±axisScale AABB missed rotation and under-fit the map.)
    const float* m = item.world.ptr();
    for (int i = 0; i < 8; ++i) {
        const float lx = (i & 1) ? 0.5f : -0.5f;
        const float ly = (i & 2) ? 0.5f : -0.5f;
        const float lz = (i & 4) ? 0.5f : -0.5f;
        const float wx = m[0] * lx + m[1] * ly + m[2] * lz + m[3];
        const float wy = m[4] * lx + m[5] * ly + m[6] * lz + m[7];
        const float wz = m[8] * lx + m[9] * ly + m[10] * lz + m[11];
        expandBoundsPoint(bounds, ayt::math::FVector3(wx, wy, wz));
    }
}

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
    float outViewProjColMajor[16],
    bool homogeneousDepth)
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

    float viewBx[16];
    bx::mtxLookAt(viewBx,
                  bx::Vec3{eye.x, eye.y, eye.z},
                  bx::Vec3{center.x, center.y, center.z},
                  bx::Vec3{up.x, up.y, up.z});

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
                const bx::Vec3 view = bx::mul(bx::Vec3{x, y, z}, viewBx);
                minX = std::fmin(minX, view.x);
                maxX = std::fmax(maxX, view.x);
                minY = std::fmin(minY, view.y);
                maxY = std::fmax(maxY, view.y);
                minZ = std::fmin(minZ, view.z);
                maxZ = std::fmax(maxZ, view.z);
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

    float projBx[16];
    bx::mtxOrtho(projBx,
                 minX, maxX,
                 minY, maxY,
                 z0, z1,
                 0.0f,
                 homogeneousDepth);

    outView = fromBgfxColumnMajor(viewBx);
    outProj = fromBgfxColumnMajor(projBx);
    bx::memCopy(outViewColMajor, viewBx, sizeof(viewBx));
    bx::memCopy(outProjColMajor, projBx, sizeof(projBx));
    // bgfx setViewTransform(view, proj) → clip = P * V * M.
    // bx::mtxMul(_result, _a, _b) computes _result = _b * _a.
    bx::mtxMul(outViewProjColMajor, viewBx, projBx);
    outViewProj = fromBgfxColumnMajor(outViewProjColMajor);
}

} // namespace

ShadowSceneBounds computeShadowSceneBounds(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes)
{
    ShadowSceneBounds bounds{};
    for (const DrawItem& item : scene.items()) {
        expandItemBounds(bounds, item, meshes);
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
    float outViewProjColMajor[16],
    bool homogeneousDepth)
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
                            outViewProjColMajor,
                            homogeneousDepth);
}

void buildDirectionalShadowMatricesFromBounds(
    const ShadowSceneBounds& bounds,
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16],
    bool homogeneousDepth)
{
    buildFromBoundsInternal(bounds,
                            lightDirection,
                            outView,
                            outProj,
                            outViewProj,
                            outViewColMajor,
                            outProjColMajor,
                            outViewProjColMajor,
                            homogeneousDepth);
}

} // namespace ayt::render::detail
