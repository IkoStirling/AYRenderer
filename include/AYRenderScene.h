#pragma once
// AYRenderScene.h — frame draw list (engine-facing, no GPU types)

#include "AYF1DiagFlags.h"
#include "AYRenderTypes.h"

#include <vector>

namespace ayt::render
{

struct DrawItem {
    MeshHandle     mesh;
    MaterialHandle material;
    ayt::math::Float4x4 world = ayt::math::Float4x4::identity();
    const ayt::math::Float4x4* boneMatrices = nullptr;
    uint32_t                   jointCount   = 0;
    int32_t                    sortKey      = 0;
};

#if AY_F1_DIAG_LIGHT
// F1 diag — enabling this grows sizeof(RenderScene) and therefore
// sizeof(RendererSubSystem) (member _scene sits before _events).
// Mismatched TU sizes → EventBusHostScope::subscribe vector crash.
struct Light {
    enum class Type : uint8_t {
        Directional = 0,
        Point       = 1,
        Spot        = 2,
    };
    Type                  type      = Type::Directional;
    ayt::math::FVector3   direction = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    ayt::math::FVector3   color     = ayt::math::FVector3(1.0f, 1.0f, 1.0f);
    float                 intensity = 1.0f;
};
#endif

class RenderScene {
public:
    void clear() {
        _items.clear();
#if AY_F1_DIAG_LIGHT
        _lights.clear();
#endif
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
        _items.push_back(item);
    }

#if AY_F1_DIAG_LIGHT
    void addLight(const Light& light) { _lights.push_back(light); }
    const std::vector<Light>& lights() const noexcept { return _lights; }
#endif

    const std::vector<DrawItem>& items() const noexcept { return _items; }
    bool empty() const noexcept { return _items.empty(); }

private:
    std::vector<DrawItem> _items;
#if AY_F1_DIAG_LIGHT
    std::vector<Light>    _lights;
#endif
};

} // namespace ayt::render
