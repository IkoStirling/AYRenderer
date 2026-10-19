#include "AYRenderer/ColorStylizationSettings.h"

namespace ayt::render {
ColorStylizationSettings colorStylizationPreset(ColorStylizationPreset preset) noexcept
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
    case ColorStylizationPreset::Pocket:
        result.enabled = true;
        result.pixelSize = 3;
        result.colorMapping = StylizationColorMapping::FixedPalette;
        result.ditherStrength = .7f;
        break;
    case ColorStylizationPreset::Sunset:
        result.enabled = true;
        result.pixelSize = 2;
        result.colorMapping = StylizationColorMapping::FixedPalette;
        result.paletteSize = 8;
        result.palette = {{{.08f,.06f,.18f},{.22f,.10f,.30f},{.45f,.17f,.37f},
            {.70f,.27f,.39f},{.91f,.43f,.36f},{1,.64f,.42f},{1,.82f,.59f},{1,.95f,.81f}}};
        result.ditherStrength = .5f;
        break;
    case ColorStylizationPreset::Noir:
        result.enabled = true;
        result.pixelationEnabled = false;
        result.colorMapping = StylizationColorMapping::Duotone;
        result.shadowColor = {.015f,.025f,.055f};
        result.highlightColor = {.91f,.95f,1};
        result.colorLevels = 3;
        break;
    case ColorStylizationPreset::Comic:
    case ColorStylizationPreset::Engraving:
        result.enabled = true;
        result.pixelationEnabled = false;
        result.posterizationEnabled = false;
        result.printPattern = preset == ColorStylizationPreset::Comic
            ? StylizationPrintPattern::Dots : StylizationPrintPattern::CrossHatch;
        result.printGamma = .65f;
        result.printStrength = preset == ColorStylizationPreset::Comic ? .75f : 1.f;
        result.outlineMode = StylizationOutlineMode::DepthNormal;
        break;
    case ColorStylizationPreset::Blueprint:
        result.enabled = true;
        result.pixelationEnabled = false;
        result.posterizationEnabled = false;
        result.colorMapping = StylizationColorMapping::Duotone;
        result.shadowColor = {.015f,.045f,.13f};
        result.highlightColor = {.65f,.88f,1};
        result.outlineMode = StylizationOutlineMode::Depth;
        result.outlineColor = {.65f,.88f,1};
        break;
    default: break;
    }
    return result;
}

ColorStylizationSettings sanitizeColorStylization(ColorStylizationSettings value) noexcept
{
    value.pixelSize = std::clamp<uint16_t>(value.pixelSize, 1, 64);
    value.colorLevels = std::clamp<uint8_t>(value.colorLevels, 2, 32);
    value.ditherStrength = std::isfinite(value.ditherStrength)
        ? std::clamp(value.ditherStrength, 0.0f, 1.0f) : 0.0f;
    const auto finiteClamp = [](float x,float low,float high,float fallback) {
        return std::isfinite(x) ? std::clamp(x,low,high) : fallback;
    };
    const auto color = [&](StylizationColor c) {
        return StylizationColor{finiteClamp(c.r,0,1,0),finiteClamp(c.g,0,1,0),finiteClamp(c.b,0,1,0)};
    };
    if (static_cast<uint8_t>(value.colorMapping)>2) value.colorMapping=StylizationColorMapping::RgbLevels;
    if (static_cast<uint8_t>(value.printPattern)>3) value.printPattern=StylizationPrintPattern::None;
    value.shadowColor=color(value.shadowColor); value.highlightColor=color(value.highlightColor);
    value.inkColor=color(value.inkColor); value.paperColor=color(value.paperColor);
    for (auto& entry:value.palette) entry=color(entry);
    value.paletteSize=std::clamp<uint8_t>(value.paletteSize,2,16);
    value.printSpacing=finiteClamp(value.printSpacing,2,64,7);
    value.printStrength=finiteClamp(value.printStrength,0,1,1);
    value.printGamma=finiteClamp(value.printGamma,.25f,4,1);
    value.printAngle=std::isfinite(value.printAngle) ? std::fmod(value.printAngle,180.0f) : 45;
    if(value.printAngle<0) value.printAngle+=180;
    if(static_cast<uint8_t>(value.outlineMode)>3) value.outlineMode=StylizationOutlineMode::None;
    value.outlineWidth=finiteClamp(value.outlineWidth,1,8,1);
    value.outlineDepthThreshold=finiteClamp(value.outlineDepthThreshold,.001f,1,.03f);
    value.outlineNormalThreshold=finiteClamp(value.outlineNormalThreshold,.01f,2,.25f);
    value.outlineLuminanceThreshold=finiteClamp(value.outlineLuminanceThreshold,.01f,1,.12f);
    value.outlineStrength=finiteClamp(value.outlineStrength,0,1,1);
    value.outlineColor=color(value.outlineColor);
    return value;
}

} // namespace ayt::render
