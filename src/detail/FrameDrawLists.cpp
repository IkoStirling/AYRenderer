#include "detail/FrameDrawLists.h"

#include "detail/Draw2D.h"
#include "detail/PassExecContext.h"

#include <algorithm>
#include <cmath>

namespace ayt::render::detail
{
namespace
{

float distanceSquared(const DrawItem& item,
                      const ayt::math::FVector3& camera) noexcept
{
    const float dx = item.world(0, 3) - camera.x;
    const float dy = item.world(1, 3) - camera.y;
    const float dz = item.world(2, 3) - camera.z;
    const float value = dx * dx + dy * dy + dz * dz;
    return std::isfinite(value) ? value : 0.0f;
}

bool transparentBefore(const SortedTransparentItem& lhs,
                       const SortedTransparentItem& rhs) noexcept
{
    if (lhs.item->sortKey != rhs.item->sortKey) {
        return lhs.item->sortKey > rhs.item->sortKey;
    }
    return lhs.distanceSquared > rhs.distanceSquared;
}

// A transformed local AABB is a convex hexahedron. Reject only when all eight
// corners lie outside one clip half-space (no divide by w / near-plane traps).
// Fail open for unknown/deformed/non-affine data, never guess unit-cube bounds.
bool outsideFrustum(const DrawItem& item, const GpuMesh& mesh,
                    const FrameContext& frame, bool& tested) noexcept
{
    tested = false;
    if (!item.allowFrustumCulling || item.outlineHull || item.payload
        || item.particleBatch || item.boneMatrices || item.jointCount
        || mesh.hasSkinWeights || mesh.hasMorphTargets || !mesh.localBoundsValid)
        return false;
    const auto& lo = mesh.localBoundsMin;
    const auto& hi = mesh.localBoundsMax;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(lo[axis]) || !std::isfinite(hi[axis])
            || lo[axis] > hi[axis]) return false;
    }
    if (item.world(3, 0) != 0.0f || item.world(3, 1) != 0.0f
        || item.world(3, 2) != 0.0f || item.world(3, 3) != 1.0f)
        return false;
    if (!std::isfinite(frame.cullingMarginX) || !std::isfinite(frame.cullingMarginY))
        return false;
    const auto m = frame.cullingViewProjection * item.world;
    bool outside[6] = {true, true, true, true, true, true};
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const float p[4] = {(corner & 1) ? hi.x : lo.x,
                            (corner & 2) ? hi.y : lo.y,
                            (corner & 4) ? hi.z : lo.z, 1.0f};
        float c[4] = {};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) c[row] += m(row, col) * p[col];
            if (!std::isfinite(c[row])) return false;
        }
        const float padX = std::max(0.0f, frame.cullingMarginX) * std::fabs(c[3]);
        const float padY = std::max(0.0f, frame.cullingMarginY) * std::fabs(c[3]);
        const float planes[6] = {c[0] + c[3] + padX, c[3] - c[0] + padX,
                                 c[1] + c[3] + padY, c[3] - c[1] + padY,
                                 frame.cullingHomogeneousDepth ? c[2] + c[3] : c[2],
                                 c[3] - c[2]};
        for (int plane = 0; plane < 6; ++plane)
            outside[plane] = outside[plane] && planes[plane] < -1.0e-5f;
    }
    tested = true;
    for (bool rejected : outside) if (rejected) return true;
    return false;
}

} // namespace

void FrameDrawLists::clear()
{
    opaque3D.clear();
    gbufferOpaque.clear();
    transparent3D.clear();
    worldLit2D.clear();
    overlay2D.clear();
    shadowCasters.clear();
    shadowBoundsItems.clear();
    selectionOutlines.clear();
    stats = {};debugDraws.clear();debugDropped=0;
}

void buildFrameDrawLists(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const std::unordered_map<uint64_t, GpuMaterial>& materials,
    const FrameContext& frame,
    FrameDrawLists& output)
{
    output.clear();
    const size_t count = scene.items().size();
    output.opaque3D.reserve(count);
    output.gbufferOpaque.reserve(count);
    output.transparent3D.reserve(count);
    output.worldLit2D.reserve(count);
    output.overlay2D.reserve(count);
    output.shadowCasters.reserve(count);
    output.shadowBoundsItems.reserve(count);
    output.selectionOutlines.reserve(count);
    output.stats.sourceItems = static_cast<uint32_t>(count);

    for (const DrawItem& item : scene.items()) {
        RenderDebugDraw* diagnostic=nullptr;
        if(frame.collectDebugDraws && item.sourceEntityId) {
            if(output.debugDraws.size()<1024) {
                output.debugDraws.emplace_back();diagnostic=&output.debugDraws.back();
                diagnostic->entityId=item.sourceEntityId;diagnostic->world=item.sourceWorld;diagnostic->mesh=item.mesh.id;
                const auto found=meshes.find(item.mesh.id);
                if(found!=meshes.end()) {
                    const auto& mesh=found->second;
                    diagnostic->deformed=item.boneMatrices || item.jointCount || mesh.hasSkinWeights || mesh.hasMorphTargets;
                    diagnostic->boundsValid=mesh.localBoundsValid;
                    if(mesh.localBoundsValid) for(int i=0;i<8;++i) diagnostic->corners[i]=item.world.transformPoint({
                        i&1?mesh.localBoundsMax.x:mesh.localBoundsMin.x,i&2?mesh.localBoundsMax.y:mesh.localBoundsMin.y,i&4?mesh.localBoundsMax.z:mesh.localBoundsMin.z});
                }
            } else ++output.debugDropped;
        }
        if (isOverlay2DItem(item)) {
            if (!item.outlineHull) {
                output.overlay2D.push_back(&item);
            }
            continue;
        }
        if (item.particleBatch && item.particleBatch->world3D
            && item.particleBatch->gpuStream && item.particleBatch->submitGpu) {
            output.transparent3D.push_back(
                {&item, distanceSquared(item, frame.cameraPosition)});
            continue;
        }
        if (!item.mesh.isValid() || !item.material.isValid()) {
            if(diagnostic) diagnostic->visibility=DebugDrawVisibility::InvalidHandles;
            ++output.stats.invalidHandles;
            continue;
        }
        const auto materialIt = materials.find(item.material.id);
        if (materialIt == materials.end()) {
            if(diagnostic) diagnostic->visibility=DebugDrawVisibility::MissingMaterial;
            ++output.stats.missingMaterials;
            continue;
        }
        const GpuMaterial& material = materialIt->second;
        const bool transparent =
            ayt::render::isTransparentBlendMode(material.blendMode);
        const bool worldLit2D = isWorldLit2DItem(item);

        if (isShadowDomainItem(item)
            && (castsShadow(item.shadowFlags)
                || receivesShadow(item.shadowFlags))) {
            output.shadowBoundsItems.push_back(&item);
        }
        if (!transparent && castsShadow(item.shadowFlags)
            && isShadowDomainItem(item)) {
            output.shadowCasters.push_back(&item);
        }

        if (worldLit2D) {
            if (!transparent) {
                output.worldLit2D.push_back(&item);
                output.gbufferOpaque.push_back(&item);
            }
            continue;
        }

        // Camera visibility must NOT truncate shadow casters/scene fitting.
        if (frame.frustumCulling) {
            bool tested = false;
            const auto meshIt = meshes.find(item.mesh.id);
            const bool rejected = meshIt != meshes.end()
                && outsideFrustum(item, meshIt->second, frame, tested);
            if(diagnostic && !tested) diagnostic->visibility=DebugDrawVisibility::CullingBypassed;
            if (tested) ++output.stats.frustumTested;
            else ++output.stats.frustumBypassed;
            if (rejected) {
                if(diagnostic) diagnostic->visibility=DebugDrawVisibility::FrustumCulled;
                ++output.stats.frustumCulled;
                continue;
            }
        }

        if (item.outlineHull) {
            output.selectionOutlines.push_back(&item);
        }
        if (transparent) {
            output.transparent3D.push_back(
                {&item, distanceSquared(item, frame.cameraPosition)});
        } else {
            output.opaque3D.push_back(&item);
            output.gbufferOpaque.push_back(&item);
        }
    }

    std::stable_sort(output.transparent3D.begin(),
                     output.transparent3D.end(), transparentBefore);
    std::stable_sort(
        output.overlay2D.begin(), output.overlay2D.end(),
        [](const DrawItem* lhs, const DrawItem* rhs) {
            return lhs->payload->packedSortKey
                < rhs->payload->packedSortKey;
        });

    output.stats.opaque3D = static_cast<uint32_t>(output.opaque3D.size());
    output.stats.transparent3D =
        static_cast<uint32_t>(output.transparent3D.size());
    output.stats.worldLit2D =
        static_cast<uint32_t>(output.worldLit2D.size());
    output.stats.overlay2D = static_cast<uint32_t>(output.overlay2D.size());
    output.stats.shadowCasters =
        static_cast<uint32_t>(output.shadowCasters.size());
    output.stats.selectionOutlines =
        static_cast<uint32_t>(output.selectionOutlines.size());

}

const FrameDrawLists& resolveFrameDrawLists(const PassExecContext& ctx,
                                            FrameDrawLists& fallback)
{
    if (ctx.drawLists != nullptr) {
        return *ctx.drawLists;
    }
    buildFrameDrawLists(ctx.scene, ctx.meshes, ctx.materials, ctx.frame,
                        fallback);
    return fallback;
}

} // namespace ayt::render::detail
