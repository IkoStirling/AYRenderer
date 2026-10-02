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
    stats = {};
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
            ++output.stats.invalidHandles;
            continue;
        }
        const auto materialIt = materials.find(item.material.id);
        if (materialIt == materials.end()) {
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

    (void)meshes; // Reserved for conservative bounds/frustum culling.
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
