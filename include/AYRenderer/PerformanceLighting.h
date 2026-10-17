#pragma once
#include <AYRenderer/RenderScene.h>
#include <span>
#include <string>

namespace ayt::render {
enum class LightPattern : uint8_t { Open, Stripes, Grid, Ring, Star, Texture };
struct PerformanceLight {
    uint32_t id = 0;
    /// Point entries require volumetric=0, Open pattern and castShadow=false.
    Light light;
    float volumetric = 1;
    LightPattern pattern = LightPattern::Open;
    float patternRotation = 0, patternScale = 1;
    /// Pattern edge blur [0,1] and circular iris opening (0,1]. Iris clips
    /// the projected pattern inside the outer beam without changing its cone.
    float patternFocus = 0, iris = 1;
    /// Renderer-owned, live texture for LightPattern::Texture. The host keeps
    /// it alive until rendering finishes; the setter rejects missing handles.
    TextureHandle goboTexture;
    /// Optional 2x2 shadow PCF radius in atlas texels, [0,4]. Zero keeps
    /// the original single-sample hard shadow; this is not area-light penumbra.
    float shadowSoftness = 0;
    int32_t priority = 0;
    /// Finite exit-aperture radius in world units, [0,1]. Zero retains the
    /// point-source angular profile. Inner/outer angles delimit core/penumbra;
    /// their radii grow as apertureRadius + axialDistance * tan(halfAngle).
    /// Shadow projection remains a point-source approximation.
    float apertureRadius = 0;
    /// Normalize the far-field angular flux during zoom relative to a 2deg core /
    /// 3deg field Spot at the same intensity. Iris/Gobo still absorb energy.
    /// Finite aperture near-field and shadows remain geometric approximations.
    bool constantFlux = false;
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
/// and surface-only Point lights; max256, sorted by descending priority then original order.
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
/// Radiant intensity multiplier shared by surface and volume; Point returns1.
float performanceLightFluxScale(const PerformanceLight& light) noexcept;
void performanceLightBounds(const PerformanceLight& light,math::FVector3& center,math::FVector3& halfExtent);
/// Conservative projected range bounds. Near-plane intersections use the full
/// viewport; scissor never clips an in-range surface or scattering sample.
LightScreenRect performanceLightRect(const PerformanceLight& light,
    const math::Float4x4& viewProjection, uint16_t width, uint16_t height);
} // namespace ayt::render
