#pragma once
#include <AYRenderer/PerformanceLighting.h>
namespace ayt::render {
class Renderer;
struct FixtureHousingSettings {
    /// Dimensions relative to the authored exit radius. Geometry does not
    /// rewrite the photometric profile or the authored light cone.
    float bodyLength=3; // [.5,10], behind the exit plane
    float hoodLength=.5f; // [0,4], ahead of the exit plane
    bool shadeLeaves=false;
    float leafAngle=0; // [-.5,.5] rad, positive folds leaves into the opening
};
enum class FixtureHousingPart : uint8_t { Body, Hood, Leaf };
struct FixtureHousingDraw {
    uint32_t id=0;
    FixtureHousingPart part=FixtureHousingPart::Body;
    math::Float4x4 world=math::Float4x4::identity();
};
struct FixtureHousingStats {uint32_t source=0,fixtures=0,parts=0;};
/// Offline transform preparation for up to256 Spot fixtures/1536 parts. Zero
/// aperture and Point entries produce no housing; a switched-off lamp retains
/// its physical body. Validation failure preserves draws/stats.
bool buildFixtureHousingDraws(std::span<const PerformanceLight> lights,
    FixtureHousingSettings settings,std::vector<FixtureHousingDraw>& draws,
    FixtureHousingStats& stats,std::string* error=nullptr);
/// Explicit owner-thread facade for opaque lamp bodies, aperture rims, hoods
/// and four optional shade leaves. Shared unit meshes/material use normal
/// opaque depth and rigid batching. Camera occlusion works independently of
/// lamp intensity. Light propagation still follows the authored cone/Gobo and
/// current shadow-map contract; these meshes are not a ray-traced lens model.
/// Renderer owns assets; recreate after shutdown/reinitialize. Settings change
/// transforms only, so retained scenes never lose previously created meshes.
class FixtureHousings final {
public:
    bool append(Renderer& renderer,RenderScene& scene,std::span<const PerformanceLight> lights,
        FixtureHousingSettings settings={},std::string* error=nullptr);
    FixtureHousingStats stats() const noexcept {return _stats;}
private:
    Renderer* _renderer=nullptr;
    MeshHandle _body,_hood,_leaf;
    MaterialHandle _material;
    std::vector<FixtureHousingDraw> _draws;
    FixtureHousingStats _stats;
};
} // namespace ayt::render
