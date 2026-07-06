#pragma once
// AYRenderScene.h — frame draw list (engine-facing, no GPU types)

#include "AYRenderTypes.h"

#include <vector>

namespace ayt::render
{

struct DrawItem {
    MeshHandle     mesh;
    MaterialHandle material;
    ayt::math::Float4x4 world = ayt::math::Float4x4::identity();
    // Phase 1 SC-01 / RD-04: null when non-skinned. When non-null,
    // `boneMatrices` points to `jointCount` contiguous mat4 entries
    // (jointCount <= 128). The renderer copies them into the
    // material's `Skeleton` UBO via setUniformBlock before draw.
    const ayt::math::Float4x4* boneMatrices = nullptr;
    uint32_t                   jointCount   = 0;
};

class RenderScene {
public:
    void clear() { _items.clear(); }

    void add(const DrawItem& item) { _items.push_back(item); }

    void add(MeshHandle mesh, MaterialHandle material,
             const ayt::math::Float4x4& world = ayt::math::Float4x4::identity())
    {
        _items.push_back(DrawItem{mesh, material, world, nullptr, 0});
    }

    // Phase 1 SC-01: skinned-draw overload. `boneMatrices` points to
    // `jointCount` entries; both must be consistent (jointCount == 0
    // iff boneMatrices == nullptr). The pointer is non-owning; the
    // caller keeps the storage alive until end of frame.
    void add(MeshHandle mesh, MaterialHandle material,
             const ayt::math::Float4x4& world,
             const ayt::math::Float4x4* boneMatrices,
             uint32_t jointCount)
    {
        DrawItem item;
        item.mesh = mesh;
        item.material = material;
        item.world = world;
        item.boneMatrices = boneMatrices;
        item.jointCount = (boneMatrices != nullptr) ? jointCount : 0;
        _items.push_back(item);
    }

    const std::vector<DrawItem>& items() const noexcept { return _items; }
    bool empty() const noexcept { return _items.empty(); }

private:
    std::vector<DrawItem> _items;
};

} // namespace ayt::render
