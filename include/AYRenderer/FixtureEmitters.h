#pragma once
#include <AYRenderer/PerformanceLighting.h>
#include <AYRenderer/VolumetricSettings.h>
namespace ayt::render {
class Renderer;
struct FixtureEmitterSettings {
    /// Unitless presentation calibration: on-axis radiance = intensity / exit
    /// area * radianceScale. Resolved physical/Legacy units come from performanceLightIntensity.
    float radianceScale=1;
    float maximumRadiance=10000; // [1,60000], before exposure/Bloom.
    float minimumPixelRadius=.75f; // [0,4], area-energy compensated footprint.
    float offAxisGlow=.001f; // [0,1], optional diffuse lens glow, front side only.
};
struct FixtureEmitterStats {
    uint32_t source=0,prepared=0,backFacing=0,culled=0,clamped=0;
};
/// Prepare at most256 oriented circular HDR exit apertures from validated Spot
/// lights. Point/zero aperture/zero intensity lights do not emit geometry.
/// Front core/field angular fade, iris, prism lobes, IES and intensity units follow the light packet;
/// optional offAxisGlow adds a faint front-hemisphere lens glow. RGB is clamped
/// before half-float HDR submission; no inverse-square factor is applied to
/// resolved surface radiance. Footprint widening preserves integrated energy.
/// Medium attenuation samples the aperture-center ray (bounded16-step animated
/// density/box quadrature), not a per-fragment participating lens simulation.
/// View/projection/viewport must match render. Invalid input preserves outputs.
/// Prepared counts are CPU candidates; GPU clipping/depth/transient budgets
/// determine actual pixels. No image Gobo, lens ghosts, starburst or housing.
bool buildFixtureEmitterBatch(std::span<const PerformanceLight> lights,
    const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
    FixtureEmitterSettings settings,const VolumetricSettings& medium,
    ParticleDrawData& output,FixtureEmitterStats& stats,std::string* error=nullptr);
/// Owner-thread facade. Renderer owns the cached disc/material/quad resources.
/// Borrowed batches survive until the next append/destruction; use OwnedCpu for
/// retained frames. Recreate after Renderer shutdown/reinitialize. Additive HDR
/// uses scene depth, no depth writes, and enters existing exposure/Bloom paths.
class FixtureEmitters final {
public:
    bool append(Renderer& renderer,RenderScene& scene,std::span<const PerformanceLight> lights,
        const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
        FixtureEmitterSettings settings={},std::string* error=nullptr);
    FixtureEmitterStats stats() const noexcept { return _stats; }
private:
    ParticleDrawData _batch;
    FixtureEmitterStats _stats;
    Renderer* _renderer=nullptr;
    MeshHandle _quad;
    TextureHandle _texture;
    MaterialHandle _material;
};
} // namespace ayt::render
