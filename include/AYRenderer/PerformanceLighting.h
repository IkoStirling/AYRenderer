#pragma once
#include <AYRenderer/RenderScene.h>
#include <span>
#include <string>

namespace ayt::render {
enum class LightPattern : uint8_t { Open, Stripes, Grid, Ring, Star };
struct PerformanceLight {
    uint32_t id = 0;
    Light light;
    float volumetric = 1;
    LightPattern pattern = LightPattern::Open;
    float patternRotation = 0, patternScale = 1;
    int32_t priority = 0;
};
struct PerformanceLightBudget {
    uint16_t surfaceLights = 64;
    uint8_t volumeLights = 16;
    uint8_t shadowLights = 4;
};
struct PerformanceLightStats {
    uint32_t sourceLights=0, surfaceDraws=0, volumeDraws=0, shadowLights=0;
    uint32_t culledLights=0, surfaceBudgetSkipped=0, volumeBudgetSkipped=0;
};
/// Renderer-owned validated immutable-for-dispatch light packet. Local Spot
/// lights only; max256, sorted by descending priority then original order.
/// Shadow projections remain a separately bounded 0..8 atlas resource.
struct PerformanceLighting {
    std::vector<PerformanceLight> lights;
    PerformanceLightBudget budget;
    SceneLights shadowSources;
    std::vector<int32_t> shadowSlots;
    PerformanceLightStats stats;
    /// Transactional validation: rejected values leave the previous packet intact.
    bool assign(std::span<const PerformanceLight> source, PerformanceLightBudget limits,
                std::string* error = nullptr);
};
struct LightScreenRect { uint16_t x=0,y=0,width=0,height=0; bool visible=false; };
void performanceLightBounds(const PerformanceLight& light,math::FVector3& center,math::FVector3& halfExtent);
/// Conservative projected range bounds. Near-plane intersections use the full
/// viewport; scissor never clips an in-range surface or scattering sample.
LightScreenRect performanceLightRect(const PerformanceLight& light,
    const math::Float4x4& viewProjection, uint16_t width, uint16_t height);
} // namespace ayt::render
