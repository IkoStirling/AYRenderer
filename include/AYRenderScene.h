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

// §P5.5 A (2026-07-23) — host-facing multi-light DataSource. Drives
// the Deferred LightingPass's accumulation loop (see
// d:\Projects\AYRuntime\AYRenderer\src\detail\LightingPass.cpp).
//
// Constraints (cutsheet §5.3 red line + §5.5 cleanup notes + §10):
//   - ❌ NEVER reintroduce RenderScene::Light (永久退休 per §5.5).
//   - ❌ NEVER add fields to FrameContext (FrameContext stays
//     pure per-frame state).
//   - ✅ Lives on the *host* side; renderer consumes a const borrowed
//     pointer via PassExecContext::sceneLights — lifetime is the
//     host's responsibility, exactly like PassExecContext::shadowPass
//     / gbufferPass / lightingPass borrowed pointer pattern (cutsheet
//     pass-lessons-from-deferred.md:329 + execution-plan.md:94-98).
//   - ✅ POD-only (LightType + FVector3 + scalars); no GPU types.
//
// Layout (light type tag introduced in §P5.5 A):
//   - `LightType { Directional=0, Point=1, Spot=2 }` enum — A only
//     emits `Directional` to the GPU (`record[i].w = 0.0`); B will
//     branch CPU-side per type (directional = -direction, point/spot
//     = +position) and activate full attenuation / spot cone math.
//     Today A keeps `type == Directional` for every default-constructed
//     Light so behaviour is byte-equivalent to B7's 8-directional path.
//   - Each Light carries direction (Directional only), position
//     (Point/Spot only — A leaves it zero), color (all types).
//   - Host fills the lights vector by calling `add(...)` per light;
//     the renderer mirrors them into the Phoskia UBO record array
//     each frame.
//
// Capacity rule:
//   - kMaxSceneLights is the hard ceiling; add() past the cap is a
//     silent no-op (host code that's already at the cap on
//     Editor Play is a debug-only concern; runtime breakage
//     avoidance is more important than loud assertion here).
//   - Default-constructed SceneLights is a no-op DataSource
//     (count=0) — exactly the same shape as B5's single light
//     driven via FrameContext (cutsheet deferred-pass.md:191
//     "1 盏方向光是 B5 ship 形态, 多光走 ctx.lights").
//
// §P5.5 A source-compat: `using DirectionalLight = Light;` is kept so
// any host code / test that still references the B7 name keeps
// compiling unchanged. `Light { type=Directional, direction=B5-default,
// color=white }` default-constructor matches the pre-A
// `DirectionalLight { direction=B5-default, color=white }` byte-for-byte.
constexpr uint32_t kMaxSceneLights = 8;

enum class LightType : uint8_t {
    Directional = 0,
    Point       = 1,
    Spot        = 2,
};

// §P5.5 A POD — single LightType byte + 3×FVector3 (= 1 + 12 + 12 + 12
// = 37 bytes; C++ struct alignment pads to 48 with LightType leading
// the layout). Field order is chosen for stability across binary
// reads (`LightType` first, then vec3 dir / pos / color) and to keep
// the B-side fields (`range` / `intensity` / `coneCosInner` /
// `coneCosOuter`) starting on a 4-byte boundary after the trailing
// vec3. B will widen this struct — A leaves headroom for 8 extra
// scalars (B's 4 float32s each fit in the next 64-byte stride).
//
// Sanity: `sizeof(Light)` should be `48` on MSVC. If the struct
// ever lands bigger than 64, the packed-vec4 UBO footprint stops
// fitting a single std140-block read on every backend; that's the
// hard cutoff.
struct Light {
    LightType             type      = LightType::Directional;
    ayt::math::FVector3   direction = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    ayt::math::FVector3   position  = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
    ayt::math::FVector3   color     = ayt::math::FVector3(1.0f, 1.0f, 1.0f);

    // Construction helper for the host pattern that pre-§P5.5 A used:
    //   sceneLights.add(DirectionalLight{ direction, color });
    // — keep accepted even after the `DirectionalLight` rename:
    static Light directional(const ayt::math::FVector3& dir,
                            const ayt::math::FVector3& col) noexcept
    {
        Light l;
        l.type      = LightType::Directional;
        l.direction = dir;
        l.color     = col;
        return l;
    }
};

static_assert(sizeof(Light) <= 64,
              "§P5.5 A: Light POD size budget — too big to fit a single "
              "packed-vec4 record field stride cleanly. B widens to 64 "
              "and beyond; if A already pushes past 64, we need to "
              "restructure the POD (cutsheet §P5.5 C.7 #6).");

// §P5.5 A source-compat alias — B7 hosts / Test_B7 references
// `DirectionalLight` directly. The alias keeps compile success
// without source edits.
using DirectionalLight = Light;

struct SceneLights {
    Light     lights[kMaxSceneLights]{};
    uint32_t  count = 0;

    // Append one light. Returns the assigned slot index, or
    // UINT32_MAX if full (host code beyond kMaxSceneLights gets a
    // soft no-op rather than OOB — debug-only concern per the
    // capacity rule above).
    uint32_t add(const Light& light) noexcept
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
