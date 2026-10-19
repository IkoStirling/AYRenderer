#pragma once

#include "AYRenderer/ColorStylizationSettings.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace ayt::render {

struct StylizationPalette {
    std::array<StylizationColor,16> colors{};
    uint8_t size=0;
    uint32_t contributingPixels=0;
};

/// Deterministically extract 2..16 display-RGB colors from tightly packed,
/// straight-alpha RGBA8. Transparent pixels are ignored; partial alpha weights
/// contributions. A 5-bit/channel histogram bounds work, then weighted median
/// cuts split weighted spread in the same RGB metric as fixed-palette rendering.
/// At most 16 Mi pixels are accepted. The result can contain fewer than requested
/// colors; a single occupied bin is duplicated to satisfy the two-color minimum.
/// Colors are ordered by luminance. Failure preserves output and reports a reason.
/// CPU only: no Renderer, resource registration, file IO or GPU upload is required.
bool extractStylizationPalette(std::span<const uint8_t> rgba8,uint32_t width,uint32_t height,
    uint8_t requestedColors,StylizationPalette& output,std::string* error=nullptr);

} // namespace ayt::render
