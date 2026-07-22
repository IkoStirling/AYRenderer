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

// §P5 B7+ (2026-07-22) — host-facing multi-light DataSource. Drives
// the Deferred LightingPass's accumulation loop (see
// d:\Projects\AYRuntime\AYRenderer\src\detail\LightingPass.cpp).
//
// Constraints (cutsheet §5.3 red line + §5.5 cleanup notes):
//   - ❌ NEVER reintroduce RenderScene::Light (永久退休 per §5.5).
//   - ❌ NEVER add fields to FrameContext (FrameContext stays
//     pure per-frame state).
//   - ✅ Lives on the *host* side; renderer consumes a const borrowed
//     pointer via PassExecContext::sceneLights — lifetime is the
//     host's responsibility, exactly like PassExecContext::shadowPass
//     / gbufferPass / lightingPass borrowed pointer pattern (cutsheet
//     pass-lessons-from-deferred.md:329 + execution-plan.md:94-98).
//   - ✅ POD-only (FVector3 + colors + scalars); no GPU types.
//
// Layout:
//   - Each DirectionalLight has dir (FVector3) + color (FVector3) —
//     Phoskia multi-light accumulation unrolls via the
//     `uniformblock Lights { vec4 dirColor[N]; vec4 dirXyz[N]; ... }`
//     pattern with N cap (see kMaxLights).
//   - Host fills the lights vector by calling `add(...)` per
//     directional; the renderer mirrors them into the Phoskia
//     uniform array each frame.
//
// Capacity rule:
//   - kMaxLights is the hard ceiling; add() past the cap is a
//     silent no-op (host code that's already at the cap on
//     Editor Play is a debug-only concern; runtime breakage
//     avoidance is more important than loud assertion here).
//   - Default-constructed SceneLights is a no-op DataSource
//     (count=0) — exactly the same shape as B5's single light
//     driven via FrameContext (cutsheet deferred-pass.md:191
//     "1 盏方向光是 B5 ship 形态, 多光走 ctx.lights").
constexpr uint32_t kMaxSceneLights = 8;

struct DirectionalLight {
    ayt::math::FVector3 direction = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    ayt::math::FVector3 color     = ayt::math::FVector3(1.0f, 1.0f, 1.0f);
};

struct SceneLights {
    DirectionalLight lights[kMaxSceneLights]{};
    uint32_t         count = 0;

    // Append one light. Returns the assigned slot index, or
    // UINT32_MAX if full (host code beyond kMaxSceneLights gets a
    // soft no-op rather than OOB — debug-only concern per the
    // capacity rule above).
    uint32_t add(const DirectionalLight& light) noexcept
    {
        if (count >= kMaxSceneLights) {
            return UINT32_MAX;
        }
        lights[count] = light;
        return count++;
    }

    bool empty() const noexcept { return count == 0; }
};

} // namespace ayt::render
