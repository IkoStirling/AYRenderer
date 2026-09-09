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

} // namespace ayt::render::detail
