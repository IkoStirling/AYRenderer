#include "detail/TextureImageLoader.h"

// AYIO pulls in Windows headers; bx defines min/max templates that conflict
// with the Win32 min/max macros unless NOMINMAX is set first.
#if defined(_WIN32) && !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <bx/bx.h>
#include <bx/allocator.h>
#include <bimg/decode.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace ayt::render::detail
{

namespace {

bx::DefaultAllocator& bimgAllocator()
{
    static bx::DefaultAllocator s_allocator;
    return s_allocator;
}

} // namespace

DecodedImage decodeUnsignedBcChannels(uint32_t width, uint32_t height,
    const uint8_t* blocks, uint32_t byteCount, bool twoChannels)
{
    if (!blocks || !width || !height || width > UINT16_MAX || height > UINT16_MAX)
        return {};
    const uint32_t paddedWidth = (width + 3u) & ~3u;
    const uint32_t paddedHeight = (height + 3u) & ~3u;
    const uint64_t expandedBytes = uint64_t(paddedWidth) * paddedHeight * 4u;
    const uint64_t expectedBlocks = uint64_t(paddedWidth / 4u) * (paddedHeight / 4u)
        * (twoChannels ? 16u : 8u);
    if (expandedBytes > 256ull * 1024u * 1024u || expectedBlocks != byteCount)
        return {};
    // Decode complete blocks into padded storage before cropping odd dimensions;
    // a block decoder must never write beyond a tightly sized partial edge row.
    std::vector<uint8_t> padded(static_cast<size_t>(expandedBytes));
    std::vector<uint8_t> pairedBlocks;
    const uint8_t* decodeBlocks = blocks;
    if (!twoChannels) {
        // This bimg version's BC4 RGBA/R8 path does not decode native red
        // values. BC5 stores two BC4 blocks: pair each red block with zero
        // green and use its supported two-channel decoder instead.
        pairedBlocks.resize(size_t(byteCount) * 2u, 0);
        for (size_t offset = 0; offset < byteCount; offset += 8u)
            std::memcpy(pairedBlocks.data() + offset * 2u, blocks + offset, 8u);
        decodeBlocks = pairedBlocks.data();
    }
    bimg::imageDecodeToRgba8(&bimgAllocator(), padded.data(), decodeBlocks,
        paddedWidth, paddedHeight, paddedWidth * 4u, bimg::TextureFormat::BC5);
    DecodedImage out;
    out.width = width;
    out.height = height;
    out.rgba8.resize(size_t(width) * height * 4u);
    for (uint32_t y = 0; y < height; ++y) {
        const auto* source = padded.data() + size_t(y) * paddedWidth * 4u;
        auto* destination = out.rgba8.data() + size_t(y) * width * 4u;
        for (uint32_t x = 0; x < width; ++x) {
            destination[x * 4u] = source[x * 4u];
            destination[x * 4u + 1u] = twoChannels ? source[x * 4u + 1u] : 0;
            destination[x * 4u + 2u] = 0;
            destination[x * 4u + 3u] = 255;
        }
    }
    return out;
}

DecodedImage decodeImageFile(const std::string& path)
{
    DecodedImage out;
    if(path.empty())return out;
    // This host-facing decoder accepts UTF-8 paths. AYIO's general File API
    // retains a narrow Windows path contract, so open the native filesystem path.
    std::ifstream file(std::filesystem::u8path(path),std::ios::binary|std::ios::ate);
    if(!file)return out;
    const auto size=file.tellg();
    if(size<=0 || uint64_t(size)>std::numeric_limits<uint32_t>::max())return out;
    std::vector<uint8_t> fileBytes(size_t(size),uint8_t{});file.seekg(0);
    file.read(reinterpret_cast<char*>(fileBytes.data()),std::streamsize(fileBytes.size()));
    if(!file)return out;

    bimg::ImageContainer* image = bimg::imageParse(
        &bimgAllocator(),
        fileBytes.data(),
        static_cast<uint32_t>(fileBytes.size()),
        bimg::TextureFormat::RGBA8);
    if (image == nullptr || image->m_width == 0 || image->m_height == 0) {
        if (image != nullptr) {
            bimg::imageFree(image);
        }
        return out;
    }

    out.width  = image->m_width;
    out.height = image->m_height;
    const size_t byteCount = static_cast<size_t>(out.width) * out.height * 4u;
    if(image->m_format!=bimg::TextureFormat::RGBA8 || byteCount>image->m_size) {
        bimg::imageFree(image);return {};
    }
    out.rgba8.resize(byteCount);
    std::memcpy(out.rgba8.data(), image->m_data, byteCount);
    bimg::imageFree(image);
    return out;
}

} // namespace ayt::render::detail
