#include "detail/ScreenshotSidecar.h"

#include "detail/TextureImageLoader.h"

#include <bimg/bimg.h>
#include <bx/file.h>

#include <cstdio>

namespace ayt::render::detail
{

namespace {

std::string stripExtensionCopy(const std::string& filePath)
{
    const size_t slash = filePath.find_last_of("/\\");
    const size_t dot   = filePath.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return filePath.substr(0, dot);
    }
    return filePath;
}

bool writePngFromRgba8(const std::string& pngPath, uint32_t width, uint32_t height,
                         const uint8_t* rgba8)
{
    if (rgba8 == nullptr || width == 0 || height == 0) {
        return false;
    }

    bx::FileWriter writer;
    bx::Error       err;
    if (!bx::open(&writer, pngPath.c_str(), false, &err)) {
        return false;
    }

    const int32_t written = bimg::imageWritePng(
        &writer, width, height, width * 4u, rgba8, bimg::TextureFormat::RGBA8, false, &err);
    bx::close(&writer);
    return written > 0;
}

} // namespace

std::string screenshotBasePath(const std::string& filePath)
{
    return stripExtensionCopy(filePath);
}

std::string screenshotTgaPath(const std::string& basePath)
{
    return basePath + ".tga";
}

std::string screenshotPngPath(const std::string& basePath)
{
    return basePath + ".png";
}

void finalizeScreenshotSidecar(const std::string& basePath)
{
    if (basePath.empty()) {
        return;
    }

    const std::string tgaPath = screenshotTgaPath(basePath);
    const std::string pngPath = screenshotPngPath(basePath);

    const DecodedImage image = decodeImageFile(tgaPath);
    if (image.rgba8.empty()) {
        std::fprintf(stderr, "[Renderer] screenshot missing or unreadable: %s\n", tgaPath.c_str());
        return;
    }

    std::fprintf(stderr, "[Renderer] screenshot saved: %s (%ux%u)\n",
                 tgaPath.c_str(), image.width, image.height);

    if (writePngFromRgba8(pngPath, image.width, image.height, image.rgba8.data())) {
        std::fprintf(stderr, "[Renderer] screenshot saved: %s (%ux%u)\n",
                     pngPath.c_str(), image.width, image.height);
    } else {
        std::fprintf(stderr, "[Renderer] screenshot PNG sidecar failed: %s\n", pngPath.c_str());
    }
}

} // namespace ayt::render::detail
