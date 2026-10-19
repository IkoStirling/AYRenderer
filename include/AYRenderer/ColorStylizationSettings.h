#pragma once
#include "AYRenderer/AnalogDisplaySettings.h"
#include "AYRenderer/TemporalFeedbackSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ayt::render {

/// RGB values are display-referred, finite and in [0,1]. Alpha is not a look control.
struct StylizationColor {
    float r = 0, g = 0, b = 0;
};

/// ValueBands quantizes max(R,G,B), preserving RGB ratios (HSV hue/saturation).
enum class StylizationColorMapping : uint8_t { RgbLevels, Duotone, FixedPalette, ValueBands };
enum class StylizationPrintPattern : uint8_t { None, Dots, Lines, CrossHatch };
/// DepthNormal uses depth alone on Forward; normals require the Deferred GBuffer.
enum class StylizationOutlineMode : uint8_t { None, Luminance, Depth, DepthNormal };

/// Owner-thread look values for independent outline and display-color stages.
/// Color order: pixel sampling, mapping/dither, print. Alpha is preserved.
struct ColorStylizationSettings {
    bool enabled = false;
    bool pixelationEnabled = true;
    uint16_t pixelSize = 6; // Physical viewport pixels per virtual pixel, 1..64.
    bool posterizationEnabled = true;
    uint8_t colorLevels = 12; // RGB/value quantizer levels, 2..32.
    float ditherStrength = 0; // 0..1; RGB/duotone quantizer or fixed palette.
    StylizationColorMapping colorMapping = StylizationColorMapping::RgbLevels;
    StylizationColor shadowColor{.035f,.055f,.12f};
    StylizationColor highlightColor{1,.9f,.68f};
    uint8_t paletteSize = 4; // 2..16; stable first-entry tie breaking.
    std::array<StylizationColor,16> palette{{
        {.059f,.22f,.059f},{.188f,.384f,.188f},{.545f,.675f,.059f},{.608f,.737f,.059f}}};
    StylizationPrintPattern printPattern = StylizationPrintPattern::None;
    float printSpacing = 7; // 2..64 physical viewport pixels.
    float printAngle = 45; // Degrees, wrapped to [0,180).
    float printStrength = 1; // 0..1; paper/ink blend, preserves source alpha.
    float printGamma = 1; // .25..4, applied to pattern luminance only.
    StylizationColor inkColor{.025f,.035f,.055f};
    StylizationColor paperColor{.96f,.93f,.83f};
    StylizationOutlineMode outlineMode = StylizationOutlineMode::None;
    float outlineWidth = 1; // Neighbor radius in physical pixels, 1..8.
    float outlineDepthThreshold = .03f; // Relative view-depth difference, .001..1.
    float outlineNormalThreshold = .25f; // 1-dot(normalA,normalB), .01..2.
    float outlineLuminanceThreshold = .12f; // Display luminance difference, .01..1.
    float outlineStrength = 1;
    StylizationColor outlineColor{.015f,.02f,.04f};
    AnalogDisplaySettings display;
    TemporalFeedbackSettings temporal;
};

enum class ColorStylizationPreset : uint8_t {
    Neutral = 0,
    PixelRetro = 1,
    LowColor = 2,
    DitherRetro = 3,
    Pocket = 4,
    Sunset = 5,
    Noir = 6,
    Comic = 7,
    Engraving = 8,
    Blueprint = 9,
    Arcade = 10,
    Broadcast = 11,
    Afterimage = 12,
    StopMotion = 13,
    TileRefresh = 14,
    TimeEcho = 15,
    CelInk = 16,
    CelPaint = 17,
};
inline constexpr uint8_t kColorStylizationPresetCount=18;

/// Construct a reusable runtime/editor look. Neutral and unknown values bypass.
ColorStylizationSettings colorStylizationPreset(ColorStylizationPreset preset) noexcept;

/// Clamp and replace non-finite runtime inputs without borrowing caller data.
ColorStylizationSettings sanitizeColorStylization(ColorStylizationSettings value) noexcept;

inline bool colorStylizationActive(const ColorStylizationSettings& value) noexcept
{
    return value.enabled && (value.posterizationEnabled
        || value.colorMapping == StylizationColorMapping::Duotone
        || value.colorMapping == StylizationColorMapping::FixedPalette
        || (value.printPattern != StylizationPrintPattern::None && value.printStrength>0)
        || (value.pixelationEnabled && value.pixelSize > 1));
}

inline bool stylizationOutlineActive(const ColorStylizationSettings& value) noexcept
{
    return value.enabled && value.outlineMode!=StylizationOutlineMode::None && value.outlineStrength>0;
}

} // namespace ayt::render
