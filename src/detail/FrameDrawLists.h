#pragma once

#include "AYRenderer/RenderScene.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ayt::render::detail
{

struct PassExecContext;

struct SortedTransparentItem {
    const DrawItem* item = nullptr;
    float distanceSquared = 0.0f;
};

struct FrameDrawListStats {
    uint32_t sourceItems = 0;
    uint32_t invalidHandles = 0;
    uint32_t missingMaterials = 0;
    uint32_t opaque3D = 0;
    uint32_t transparent3D = 0;
    uint32_t worldLit2D = 0;
    uint32_t overlay2D = 0;
    uint32_t shadowCasters = 0;
    uint32_t selectionOutlines = 0;
};

// Immutable per-frame classification shared by geometry passes. Lists retain
// pointers into RenderScene, whose lifetime already covers synchronous
// Renderer::render(). Stable sorting preserves source order for equal keys.
struct FrameDrawLists {
    std::vector<const DrawItem*> opaque3D;
    std::vector<const DrawItem*> gbufferOpaque;
    std::vector<SortedTransparentItem> transparent3D;
    std::vector<const DrawItem*> worldLit2D;
    std::vector<const DrawItem*> overlay2D;
    std::vector<const DrawItem*> shadowCasters;
    std::vector<const DrawItem*> shadowBoundsItems;
    std::vector<const DrawItem*> selectionOutlines;
    FrameDrawListStats stats{};

    void clear();
};

void buildFrameDrawLists(
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const std::unordered_map<uint64_t, GpuMaterial>& materials,
    const FrameContext& frame,
    FrameDrawLists& output);

// Legacy/unit contexts may omit the shared list. They receive an equivalent
// local build, while the product Renderer always supplies one shared result.
const FrameDrawLists& resolveFrameDrawLists(const PassExecContext& ctx,
                                            FrameDrawLists& fallback);

} // namespace ayt::render::detail
