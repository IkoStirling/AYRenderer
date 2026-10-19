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
