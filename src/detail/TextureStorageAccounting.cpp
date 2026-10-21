#include "TextureStorageAccounting.h"
#include <bimg/bimg.h>
#include <algorithm>
#include <limits>

namespace ayt::render::detail {
// bgfx's public texture enum mirrors its bimg dependency. Guard representative
// boundaries and the count before using that shared format-table ABI.
static_assert(unsigned(bgfx::TextureFormat::Count)==unsigned(bimg::TextureFormat::Count));
static_assert(unsigned(bgfx::TextureFormat::BC1)==unsigned(bimg::TextureFormat::BC1));
static_assert(unsigned(bgfx::TextureFormat::BC7)==unsigned(bimg::TextureFormat::BC7));
static_assert(unsigned(bgfx::TextureFormat::RGBA8)==unsigned(bimg::TextureFormat::RGBA8));
static_assert(unsigned(bgfx::TextureFormat::RGBA32F)==unsigned(bimg::TextureFormat::RGBA32F));
static_assert(unsigned(bgfx::TextureFormat::D24S8)==unsigned(bimg::TextureFormat::D24S8));
const char* textureStorageFormatName(bgfx::TextureFormat::Enum format) noexcept
{
    if(static_cast<uint32_t>(format)>=static_cast<uint32_t>(bgfx::TextureFormat::Count))return "unknown";
    return bimg::getName(static_cast<bimg::TextureFormat::Enum>(format));
}
bool textureLogicalStorageBytes(uint32_t width, uint32_t height, uint32_t mipCount,
    bool cube, bgfx::TextureFormat::Enum format, uint64_t& output) noexcept
{
    if (!width || !height || !mipCount || (cube && width != height)
        || static_cast<uint32_t>(format) >= static_cast<uint32_t>(bgfx::TextureFormat::Count)
        || format == bgfx::TextureFormat::Unknown || format == bgfx::TextureFormat::UnknownDepth)
        return false;
    uint32_t maximumLevels = 1;
    for (uint32_t extent = std::max(width, height); extent > 1; extent /= 2) ++maximumLevels;
    if (mipCount > maximumLevels) return false;
    const auto imageFormat = static_cast<bimg::TextureFormat::Enum>(format);
    if (!bimg::isValid(imageFormat)) return false;
    const auto& block = bimg::getBlockInfo(imageFormat);
    if (!block.blockWidth || !block.blockHeight || !block.blockSize) return false;
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    uint64_t total = 0;
    for (uint32_t level = 0; level < mipCount; ++level) {
        const uint64_t columns = std::max<uint64_t>(block.minBlockX,
            (uint64_t(width) + block.blockWidth - 1) / block.blockWidth);
        const uint64_t rows = std::max<uint64_t>(block.minBlockY,
            (uint64_t(height) + block.blockHeight - 1) / block.blockHeight);
        if (columns > maximum / rows) return false;
        uint64_t bytes = columns * rows;
        if (bytes > maximum / block.blockSize) return false;
        bytes *= block.blockSize;
        const uint64_t faces = cube ? 6 : 1;
        if (bytes > maximum / faces) return false;
        bytes *= faces;
        if (bytes > maximum - total) return false;
        total += bytes;
        width = std::max(1u, width / 2); height = std::max(1u, height / 2);
    }
    output = total;
    return true;
}
} // namespace ayt::render::detail
