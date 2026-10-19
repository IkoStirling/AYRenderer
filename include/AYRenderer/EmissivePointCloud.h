#pragma once
#include <AYRenderer/RenderScene.h>
#include <AYRenderer/PerformanceLighting.h>
#include <span>
#include <string>
namespace ayt::render {
class Renderer;
struct EmissivePoint {
    uint32_t id=0;
    math::FVector3 position{0,0,0};
    math::FVector3 color{1,1,1};
    float intensity=4,radius=.05f;
};
/// Optional surface-light approximation, independent of visible-point sampling.
/// Hard cells sum linear RGB at a luminance-weighted center; smoothCells
/// distributes it continuously to fixed grid nodes.
struct EmissivePointLightingSettings {
    uint16_t maxLights=16; // [0,64]; zero disables aggregation.
    float cellSize=8, range=20, intensityScale=.01f;
    /// Trilinearly distribute RGB to eight world-anchored grid nodes. False
    /// retains hard cells and energy-weighted centers. No temporal state here.
    bool smoothCells=false;
    /// Copied to generated Point lights; same screen-space bounds/limitations.
    float contactDistance=0,contactThickness=.2f,contactBias=.02f,contactStrength=1;
};
struct EmissivePointLightingStats {
    uint32_t source=0, contributing=0, occupiedCells=0, selectedLights=0, budgetSkipped=0, clampedCells=0;
    uint32_t fadingLights=0,deferredCells=0;
    bool historyReset=false;
};
/// Build at most64 unshadowed, non-volumetric Point lights from <=65536 points.
/// Brightest cells win, ties use lexicographic cell coordinates; stable IDs fix
/// accumulation order. No camera/visibility budget affects illumination. Cell
/// transitions and budget changes can pop in hard mode; smoothCells and the
/// optional history address these separately. Contact occlusion is view-dependent.
/// Invalid values/duplicate IDs leave output and stats intact. RGB above1e9
/// per cell clamps with an explicit counter. Output IDs are local [0,N).
bool buildEmissivePointLights(std::span<const EmissivePoint> points,
    EmissivePointLightingSettings settings,std::vector<PerformanceLight>& output,
    EmissivePointLightingStats& stats,std::string* error=nullptr);
/// Optional owner-thread rank transition cache for smoothCells=true. Keeps at
/// most maxLights slots, fades departing cells out before admitting new cells.
/// Reset on seek/edit; backward time, gaps>0.25s, settings changes and all-black
/// input reset automatically. Repeated timestamps do not advance fades.
/// Fades may temporarily reduce energy; no per-point shadows or GI implied.
class EmissivePointLightingHistory final {
public:
    bool update(std::span<const EmissivePoint> points,EmissivePointLightingSettings settings,
        double time,float transitionSeconds,std::vector<PerformanceLight>& output,
        EmissivePointLightingStats& stats,std::string* error=nullptr);
    void reset() noexcept { _slots.clear();_valid=false; }
private:
    struct Slot { PerformanceLight light;float gain=1; };
    std::vector<Slot> _slots;
    EmissivePointLightingSettings _settings;
    double _time=0;
    float _transition=0;
    bool _valid=false;
};
struct EmissivePointSettings {
    float minimumPixelRadius=.75f;
    bool preserveEnergy=true;
    /// Maximum visible points. When exceeded, stable IDs choose a spatially
    /// unbiased deterministic subset so input order does not shape the show.
    uint32_t maxPoints=65536;
    /// Prefer one hardware instance per visible point when supported; the
    /// existing CPU quad path remains the fallback and comparison mode.
    bool preferInstancing=true;
};
struct EmissivePointStats {
    uint32_t source=0,visible=0,culled=0,budgetSkipped=0,batches=0;
    bool instanced=false;
};
/// CPU quad preparation for deterministic tests and non-instanced backends.
/// No births/lifetimes/RNG and no SceneLights per point. Input must be finite;
/// failure leaves output intact. Does not promise a particular frame rate.
bool buildEmissivePointBatches(std::span<const EmissivePoint> points,
    const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
    EmissivePointSettings settings,std::vector<ParticleDrawData>& batches,EmissivePointStats& stats,
    std::string* error=nullptr);
/// Deterministic packed-instance preparation used by the GPU path and tests.
/// Keeps the same culling, stable-ID budget and subpixel energy as CPU quads.
bool buildEmissivePointInstanceBatches(std::span<const EmissivePoint> points,
    const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
    EmissivePointSettings settings,std::vector<ParticleDrawData>& batches,EmissivePointStats& stats,
    std::string* error=nullptr);
/// Persistent owner-thread facade for reusable HDR point-cloud batches.
class EmissivePointCloud final {
public:
    /// Renderer owns cached GPU assets. Prepare/append/render on its owner thread.
    /// Borrowed scene payloads live until this object's next append/destruction;
    /// use an OwnedCpu RenderScene when retaining frames. Renderer must outlive use.
    /// Recreate this object after Renderer shutdown/reinitialize.
    bool append(Renderer& renderer,RenderScene& scene,std::span<const EmissivePoint> points,
        const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
        EmissivePointSettings settings={},std::string* error=nullptr);
    EmissivePointStats stats() const noexcept { return _stats; }
private:
    std::vector<ParticleDrawData> _batches;
    EmissivePointStats _stats;
    Renderer* _renderer=nullptr;
    MeshHandle _quad;
    TextureHandle _texture;
    MaterialHandle _material;
    MaterialHandle _instancedMaterial;
};
} // namespace ayt::render
