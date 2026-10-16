#pragma once
#include <AYRenderer/RenderScene.h>
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
struct EmissivePointSettings {
    float minimumPixelRadius=.75f;
    bool preserveEnergy=true;
    /// Maximum visible points. When exceeded, stable IDs choose a spatially
    /// unbiased deterministic subset so input order does not shape the show.
    uint32_t maxPoints=65536;
};
struct EmissivePointStats { uint32_t source=0,visible=0,culled=0,budgetSkipped=0,batches=0; };
/// CPU-owned HDR billboard batches, shared shader/material, per-point color.
/// No births/lifetimes/RNG and no SceneLights per point. Input must be finite;
/// failure leaves output intact. Does not promise a particular frame rate.
bool buildEmissivePointBatches(std::span<const EmissivePoint> points,
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
};
} // namespace ayt::render
