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

// BC4/BC5 have no native sRGB view. Expand their unsigned channels without
// inventing blue/alpha values, for an RGBA8 sRGB upload. Input is one exact mip;
// dimensions fit the GPU's 16-bit interface, expanded storage is capped at 256 MiB.
DecodedImage decodeUnsignedBcChannels(uint32_t width, uint32_t height,
    const uint8_t* blocks, uint32_t byteCount, bool twoChannels);

} // namespace ayt::render::detail
