#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ayt::render {

/// Display-referred, viewport-local color effects. Processing order is pixel
/// sampling, ordered dither, then RGB quantization. Alpha is preserved.
struct ColorStylizationSettings {
    bool enabled = false;
    bool pixelationEnabled = true;
    uint16_t pixelSize = 6; // Physical viewport pixels per virtual pixel, 1..64.
    bool posterizationEnabled = true;
    uint8_t colorLevels = 12; // Levels per RGB channel, 2..32.
    float ditherStrength = 0; // 0..1; used only with posterization.
};

enum class ColorStylizationPreset : uint8_t {
    Neutral = 0,
    PixelRetro = 1,
    LowColor = 2,
    DitherRetro = 3,
};

/// Construct a reusable runtime/editor look. Neutral and unknown values bypass.
inline ColorStylizationSettings colorStylizationPreset(ColorStylizationPreset preset) noexcept
{
    ColorStylizationSettings result;
    switch (preset) {
    case ColorStylizationPreset::PixelRetro:
        result.enabled = true;
        break;
    case ColorStylizationPreset::LowColor:
        result.enabled = true;
        result.pixelationEnabled = false;
        result.colorLevels = 4;
        break;
    case ColorStylizationPreset::DitherRetro:
        result.enabled = true;
        result.pixelSize = 3;
        result.colorLevels = 4;
        result.ditherStrength = 1;
        break;
    default: break;
    }
    return result;
}

inline ColorStylizationSettings sanitizeColorStylization(ColorStylizationSettings value) noexcept
{
    value.pixelSize = std::clamp<uint16_t>(value.pixelSize, 1, 64);
    value.colorLevels = std::clamp<uint8_t>(value.colorLevels, 2, 32);
    value.ditherStrength = std::isfinite(value.ditherStrength)
        ? std::clamp(value.ditherStrength, 0.0f, 1.0f) : 0.0f;
    return value;
}

inline bool colorStylizationActive(const ColorStylizationSettings& value) noexcept
{
    return value.enabled && (value.posterizationEnabled
        || (value.pixelationEnabled && value.pixelSize > 1));
}

} // namespace ayt::render
