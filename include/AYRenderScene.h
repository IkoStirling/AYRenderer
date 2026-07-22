#pragma once
// AYRenderScene.h — frame draw list (engine-facing, no GPU types)

#include "AYRenderTypes.h"

#include <vector>

namespace ayt::render
{

// §5.5 cleanup (2026-07-22) — RenderScene no longer holds an F1-diagnostic
// Light struct / _lights vector / addLight() API. That storage was the
// §5.5 PR-F1' C' forbidden combo (RenderScene::Light combined with
// default-on Shadow and FrameContext shadow writeback). E5 ships
// default-on Shadow WITHOUT a Light struct or writeback; the storage
// is permanently retired. Hosts that want to drive directional light
// state still call Renderer::setDirectionalLight(dir, color) which
// feeds FrameContext::lightDirection (the existing primitive) — no
// Light struct ever needed on the render path.
struct DrawItem {
    MeshHandle     mesh;
    MaterialHandle material;
    ayt::math::Float4x4 world = ayt::math::Float4x4::identity();
    const ayt::math::Float4x4* boneMatrices = nullptr;
    uint32_t                   jointCount   = 0;
    int32_t                    sortKey      = 0;
    ShadowFlags                shadowFlags  = kShadowCastAndReceive;
};

class RenderScene {
public:
    void clear() {
        _items.clear();
    }

    void add(const DrawItem& item) { _items.push_back(item); }

    void add(MeshHandle mesh, MaterialHandle material,
             const ayt::math::Float4x4& world = ayt::math::Float4x4::identity())
    {
        DrawItem item;
        item.mesh         = mesh;
        item.material     = material;
        item.world        = world;
        item.boneMatrices = nullptr;
        item.jointCount   = 0;
        item.sortKey      = 0;
        item.shadowFlags  = kShadowCastAndReceive;
        _items.push_back(item);
    }

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
        item.shadowFlags = kShadowCastAndReceive;
        _items.push_back(item);
    }

    const std::vector<DrawItem>& items() const noexcept { return _items; }
    bool empty() const noexcept { return _items.empty(); }

private:
    std::vector<DrawItem> _items;
};

} // namespace ayt::render
