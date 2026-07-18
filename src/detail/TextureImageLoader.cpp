#include "detail/TextureImageLoader.h"

// AYIO pulls in Windows headers; bx defines min/max templates that conflict
// with the Win32 min/max macros unless NOMINMAX is set first.
#if defined(_WIN32) && !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <bx/bx.h>
#include <bx/allocator.h>
#include <bimg/decode.h>

#include <ayio/File.h>

#include <cstring>

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
    if (path.empty() || !ayt::io::File::exists(path)) {
        return out;
    }

    const std::vector<uint8_t> fileBytes = ayt::io::File::readAllBytes(path);
    if (fileBytes.empty()) {
        return out;
    }

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
    out.rgba8.resize(byteCount);
    std::memcpy(out.rgba8.data(), image->m_data, byteCount);
    bimg::imageFree(image);
    return out;
}

} // namespace ayt::render::detail
