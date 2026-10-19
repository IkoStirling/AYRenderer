#pragma once

#include "AYRenderer/ColorStylizationSettings.h"
#include <string>
#include <string_view>

namespace ayt::render {

/// Experimental .rsl look document. Writes v4, reads complete v1/v2/v3/v4; old
/// documents migrate with missing display/temporal groups disabled. No referenced
/// assets, Scene mutation or automatic ResourceManager/Cook/Pak integration.
/// Serialization sanitizes runtime values. Parsing rejects malformed, unknown,
/// incomplete or out-of-range documents (64 KiB limit), preserving output on failure.
/// Numeric controls decode as Float32; valid short JSON decimals are accepted.
std::string serializeColorStylizationLook(const ColorStylizationSettings& settings);
bool parseColorStylizationLook(std::string_view text, ColorStylizationSettings& output,
                              std::string* error = nullptr);
/// Explicit native-host UTF-8 file access. Save requires .rsl and creates parents;
/// uses AYIO UTF-8 atomic replacement. Editor callers additionally constrain project paths.
bool loadColorStylizationLook(const std::string& path, ColorStylizationSettings& output,
                             std::string* error = nullptr);
bool saveColorStylizationLook(const std::string& path, const ColorStylizationSettings& settings,
                             std::string* error = nullptr);

} // namespace ayt::render
