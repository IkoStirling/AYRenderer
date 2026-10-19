#pragma once
#include <cstdint>
namespace ayt::render {
/// Optional display simulation, after the artistic color/print stages. Physical
/// screen coordinates keep the mask and scanlines stable under image warping.
/// No history is retained; callers can animate noise by changing the seed.
struct AnalogDisplaySettings {
    bool enabled=false;
    float scanlineStrength=.25f; // 0..1, alternating physical rows.
    float maskStrength=.15f; // 0..1, repeating RGB phosphor columns.
    float chromaticOffset=.75f; // 0..4 physical pixels.
    float curvature=.06f; // 0...25 barrel warp; out-of-image RGB is black.
    float vignetteStrength=.2f; // 0..1.
    float grainStrength=0; // 0...25 display RGB amplitude.
    uint16_t noiseSeed=0;
};
AnalogDisplaySettings sanitizeAnalogDisplay(AnalogDisplaySettings value) noexcept;
bool analogDisplayActive(const AnalogDisplaySettings& value) noexcept;
} // namespace ayt::render
