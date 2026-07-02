#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::render::detail
{

struct DecodedImage {
    uint32_t width  = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba8;
    bool isValid() const noexcept { return width > 0 && height > 0 && !rgba8.empty(); }
};

DecodedImage decodeImageFile(const std::string& path);

} // namespace ayt::render::detail
