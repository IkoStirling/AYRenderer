#include "detail/TextureImageLoader.h"

// AYIO pulls in Windows headers; bx defines min/max templates that conflict
// with the Win32 min/max macros unless NOMINMAX is set first.
#if defined(_WIN32) && !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <bx/bx.h>
#include <bx/allocator.h>
#include <bx/error.h>
#include <bimg/decode.h>
#include <stb_image.h>

#include <cstring>
#include <algorithm>
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

DecodedImage decodeBcColor(uint32_t width, uint32_t height,
    const uint8_t* blocks, uint32_t byteCount, BcColorFormat format)
{
    if (!blocks || !width || !height || width > UINT16_MAX || height > UINT16_MAX) return {};
    bimg::TextureFormat::Enum source;
    uint32_t blockSize = 0;
    switch (format) {
    case BcColorFormat::BC1: source = bimg::TextureFormat::BC1; blockSize = 8; break;
    case BcColorFormat::BC3: source = bimg::TextureFormat::BC3; blockSize = 16; break;
    case BcColorFormat::BC7: source = bimg::TextureFormat::BC7; blockSize = 16; break;
    default: return {};
    }
    const uint32_t paddedWidth = (width+3u)&~3u, paddedHeight = (height+3u)&~3u;
    const uint64_t paddedBytes = uint64_t(paddedWidth)*paddedHeight*4u;
    if (paddedBytes > 256ull*1024u*1024u
        || uint64_t(paddedWidth/4u)*(paddedHeight/4u)*blockSize != byteCount) return {};
    std::vector<uint8_t> padded(static_cast<size_t>(paddedBytes));
    bimg::imageDecodeToRgba8(&bimgAllocator(),padded.data(),blocks,paddedWidth,paddedHeight,paddedWidth*4u,source);
    DecodedImage out;
    out.width = width; out.height = height; out.rgba8.resize(size_t(width)*height*4u);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(out.rgba8.data()+size_t(y)*width*4u,padded.data()+size_t(y)*paddedWidth*4u,size_t(width)*4u);
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
    constexpr uint64_t decodeLimit = 256ull*1024u*1024u;
    if(size<=0 || uint64_t(size)>decodeLimit)return out;
    std::vector<uint8_t> fileBytes(size_t(size),uint8_t{});file.seekg(0);
    file.read(reinterpret_cast<char*>(fileBytes.data()),std::streamsize(fileBytes.size()));
    if(!file)return out;

    // Inspect dimensions without expanding pixels first. The container parser
    // borrows encoded data; stb's info path handles the common raster formats.
    // A tiny malicious header must not trigger a multi-gigabyte decode before
    // the resource bridge gets a chance to validate its dimensions.
    bimg::ImageContainer header{};
    bx::Error headerError;
    if (bimg::imageParse(header,fileBytes.data(),static_cast<uint32_t>(fileBytes.size()),&headerError)
        && headerError.isOk()) {
        if (!header.m_width || !header.m_height || header.m_depth > 1
            || header.m_numLayers > 1 || header.m_cubeMap || header.m_numMips > 32) return {};
        uint64_t expanded = 0;
        uint32_t w = header.m_width,h = header.m_height;
        for (uint32_t level = 0; level < std::max(1u,uint32_t(header.m_numMips)); ++level) {
            const uint64_t pixels = uint64_t(w)*h;
            if (pixels > decodeLimit/4u) return {};
            expanded += pixels*4u;
            if (expanded > decodeLimit) return {};
            w = std::max(1u,w/2); h = std::max(1u,h/2);
        }
    } else {
        int w=0,h=0,components=0;
        if (!stbi_info_from_memory(fileBytes.data(),static_cast<int>(fileBytes.size()),&w,&h,&components)
            || w <= 0 || h <= 0 || uint64_t(w)*h*4u > decodeLimit) return {};
    }

    bx::Error decodeError;
    bimg::ImageContainer* image = bimg::imageParse(
        &bimgAllocator(),
        fileBytes.data(),
        static_cast<uint32_t>(fileBytes.size()),
        bimg::TextureFormat::RGBA8,&decodeError);
    if (image == nullptr || !decodeError.isOk() || image->m_width == 0 || image->m_height == 0
        || uint64_t(image->m_width)*image->m_height > decodeLimit/4u) {
        if (image != nullptr) {
            bimg::imageFree(image);
        }
        return out;
    }

    out.width  = image->m_width;
    out.height = image->m_height;
    const size_t byteCount = static_cast<size_t>(out.width) * out.height * 4u;
    if(image->m_format!=bimg::TextureFormat::RGBA8 || byteCount>image->m_size || image->m_size>decodeLimit) {
        bimg::imageFree(image);return {};
    }
    out.rgba8.resize(byteCount);
    std::memcpy(out.rgba8.data(), image->m_data, byteCount);
    bimg::imageFree(image);
    return out;
}

} // namespace ayt::render::detail
