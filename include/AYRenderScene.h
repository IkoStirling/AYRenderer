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
// = 37 bytes; C++ struct alignment pads to 40 on MSVC with no trailing
// vec3 padding needed). Field order is chosen for stability across
// binary reads (`LightType` first, then vec3 dir / pos / color).
//
// §P5.5 B (2026-07-23) — POD widened with per-light params
// (`range` / `intensity` / `coneCosInner` / `coneCosOuter`) and
// `spotDirection` for Spot light type. A's 4 fields stay intact;
// the new fields are appended in std140-friendly order (4 × float32
// then 1 × FVector3 = 12B) so the C++ struct stays 4-byte aligned
// and `sizeof(Light) ≈ 68` on MSVC. `static_assert` ceiling bumped
// from `≤ 64` to `≤ 96` to give room for the B fields; std140
// single-block read breaks on some backends around 128B, so `≤ 96`
// is the last comfortable plateau before a POD restructure.
//
// Field layout (verified at compile time on MSVC):
//   - bytes [0,  4): LightType byte + 3 padding (struct alignment
//                     forces next field to offset 4)
//   - bytes [4, 16): direction (FVector3, 12B)
//   - bytes [16,28): position (FVector3, 12B)
//   - bytes [28,40): color (FVector3, 12B)
//   - bytes [40,44): range (float32)
//   - bytes [44,48): intensity (float32)
//   - bytes [48,52): coneCosInner (float32)
//   - bytes [52,56): coneCosOuter (float32)
//   - bytes [56,68): spotDirection (FVector3, 12B)
//   - bytes [68,72): trailing pad to next 8-byte boundary
//                  → final sizeof = 72B (well under ≤ 96 cap)
struct Light {
    LightType             type      = LightType::Directional;
    ayt::math::FVector3   direction = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    ayt::math::FVector3   position  = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
    ayt::math::FVector3   color     = ayt::math::FVector3(1.0f, 1.0f, 1.0f);
    // §P5.5 B (2026-07-23) — Point + Spot per-light params.
    // Directional lights leave all 4 fields default (range = 0
    // = ignored, intensity = 1.0 = no attenuation scaling,
    // coneCosInner/Outer = 0 = not a spot). Phoskia FS gates on
    // `dirs[i].w = LightType` before reading these (cutsheet
    // §P5.5 C #1 footgun's fix — forward-declared in A's CPU
    // pack code at LightingPass.cpp:633).
    float                 range          = 0.0f;   // Point/Spot cutoff (m)
    float                 intensity      = 1.0f;   // Point/Spot strength multiplier
    float                 coneCosInner   = 0.0f;   // Spot inner cone cos
    float                 coneCosOuter   = 0.0f;   // Spot outer cone cos
    ayt::math::FVector3   spotDirection  = ayt::math::FVector3(0.0f, -1.0f, 0.0f);

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

    // §P5.5 B (2026-07-23) — Point factory (mirror `directional()`).
    // Defaults spotDirection / coneCosInner / coneCosOuter to neutral
    // values; Point lights ignore these (FS branch on `ltype < 1.5`
    // never reads `spotDir[]` or `coneCos*`).
    static Light point(const ayt::math::FVector3& pos,
                       float range,
                       float intensity,
                       const ayt::math::FVector3& col) noexcept
    {
        Light l;
        l.type        = LightType::Point;
        l.position    = pos;
        l.range       = range;
        l.intensity   = intensity;
        l.color       = col;
        return l;
    }

    // §P5.5 B (2026-07-23) — Spot factory (mirror `directional()`).
    // Spot direction is `dir` (normalized inside the FS); cone falloff
    // uses `coneCosInner` (full-intensity threshold) and
    // `coneCosOuter` (zero-intensity threshold) with smoothstep
    // interpolation between them.
    static Light spot(const ayt::math::FVector3& pos,
                      const ayt::math::FVector3& dir,
                      float range,
                      float intensity,
                      float coneCosInner,
                      float coneCosOuter,
                      const ayt::math::FVector3& col) noexcept
    {
        Light l;
        l.type           = LightType::Spot;
        l.position       = pos;
        l.spotDirection  = dir;
        l.range          = range;
        l.intensity      = intensity;
        l.coneCosInner   = coneCosInner;
        l.coneCosOuter   = coneCosOuter;
        l.color          = col;
        return l;
    }
};

static_assert(sizeof(Light) <= 96,
              "§P5.5 B: Light POD size budget — widen only when forced by "
              "future Spot params. If this trips, restructure the POD "
              "(don't push past 96B; std140 single-block read breaks on "
              "some backends around 128B).");

// §P5.5 A source-compat alias — B7 hosts / Test_B7 references
// `DirectionalLight` directly. The alias keeps compile success
// without source edits.
using DirectionalLight = Light;

// §Skybox0 (2026-07-23) — host-facing Skybox DataSource.
// Single equirect texture (current default) OR single cube map
// (future B cut). Lives on the host; renderer borrows via
// PassExecContext::skySource — mirror SceneLights borrowed-ptr
// pattern.
//
// Constraints (cutsheet §10 pass-lessons-from-deferred.md + §Skybox0):
//   - ❌ NO bgfx:: types in this header (public surface; mirror Light).
//   - ❌ NO RenderScene mutation (sky is a *scene-exterior* property,
//     like SceneLights — RenderScene holds only DrawItems).
//   - ✅ POD-only (enum + TextureHandle + uint64_t); no allocators,
//     no strings, no GPU types.
//   - ✅ Borrowed-ptr pattern: host owns the SkySource instance,
//     passes its address into Renderer::setSkySource(); the renderer
//     reads the pointer through PassExecContext::skySource per frame
//     without copying. Lifetime contract: the SkySource must remain
//     valid across any in-flight render() call (easiest lifetime =
//     a host member, populated once at startup or per scene change).
//
// MVP A scope (equirect only, 2026-07-23):
//   - `kind == Equirect` ⇒ SkyboxPass renders via `texture2d skyEquirect`.
//   - `kind == CubeMap`  ⇒ SkyboxPass early-returns 0 today (enum
//     placeholder; B-cut will add the samplerCube path).
//   - `cubeReserve` is reserved for the B-cut handle. A leaves it 0.
enum class SkySourceKind : uint8_t {
    Equirect = 0,  // 2D panoramic sphere (current default).
    CubeMap  = 1,  // samplerCube — reserved for §Skybox0-B cut.
};

struct SkySource {
    SkySourceKind kind = SkySourceKind::Equirect;
    TextureHandle equirect{};  // 2D equirect texture (kind==Equirect)
    // Cube sampler handle reserved for future B cut; A leaves it 0
    // (host can populate later via Renderer::setSkySourceCube() — not
    // in A scope).
    uint64_t cubeReserve = 0;

    bool hasEquirect() const noexcept { return equirect.isValid(); }
    bool isActive()    const noexcept {
        return kind == SkySourceKind::Equirect && hasEquirect();
    }
};

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
