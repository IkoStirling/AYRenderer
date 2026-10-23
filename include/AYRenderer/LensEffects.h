#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ayt::render {
/// Camera-space artistic glare from the current frame's visible HDR highlights,
/// before exposure/tonemap. Disabled by default. Uses Bloom extraction even when
/// Bloom contribution is zero; no source-ID tracking, diffraction or lens ray tracing.
struct LensEffectsSettings {
    bool enabled=false;
    float starStrength=.2f;       // [0,4]
    float starLength=.06f;        // [0.001,0.25] of viewport height
    float starRotation=0;         // radians
    uint8_t starAxes=3;           // [1,4], two opposing rays per axis
    float ghostStrength=.1f;      // [0,4], three center-reflected ghosts
    float dispersion=.003f;      // [0,.02], chromatic separation
    float dirtStrength=0;         // [0,1], fixed procedural sensor/lens pattern
};
/// Same finite/clamped settings for graph planning and GPU binding.
inline LensEffectsSettings sanitizeLensEffects(LensEffectsSettings s) noexcept {
    const auto finite=[](float x,float fallback,float lo,float hi){return std::isfinite(x)?std::clamp(x,lo,hi):fallback;};
    s.starStrength=finite(s.starStrength,0,0,4);s.ghostStrength=finite(s.ghostStrength,0,0,4);
    s.starLength=finite(s.starLength,.06f,.001f,.25f);s.starRotation=finite(s.starRotation,0,-100000,100000);
    s.starAxes=std::clamp<uint8_t>(s.starAxes,1,4);s.dispersion=finite(s.dispersion,0,0,.02f);s.dirtStrength=finite(s.dirtStrength,0,0,1);
    return s;
}
inline bool lensEffectsActive(LensEffectsSettings s) noexcept {
    s=sanitizeLensEffects(s);return s.enabled&&(s.starStrength>0||s.ghostStrength>0);
}
} // namespace ayt::render
