#include "detail/ScreenshotSidecar.h"

#include "detail/TextureImageLoader.h"

#include <bimg/bimg.h>
#include <bx/file.h>

#include <cstdio>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

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

bool finalizeScreenshotSidecar(const std::string& captureBase,const std::string& outputBase)
{
    if(captureBase.empty() || outputBase.empty())return false;

    const std::string tgaPath = screenshotTgaPath(captureBase);
    const std::string pngPath = screenshotPngPath(outputBase);

    // bgfx's default callback writes an uncompressed 32-bit TGA. Require the
    // complete pixel payload before decoding; its file can exist while still writing.
    std::ifstream input(tgaPath,std::ios::binary|std::ios::ate);
    if(!input)return false;
    const auto bytes=input.tellg();input.seekg(0);
    uint8_t header[18]{};input.read(reinterpret_cast<char*>(header),18);
    if(!input || header[2]!=2 || header[1]!=0 || (header[16]!=24 && header[16]!=32))return false;
    const uint32_t width=header[12]|(uint32_t(header[13])<<8),height=header[14]|(uint32_t(header[15])<<8);
    const uint64_t expected=18u+header[0]+uint64_t(width)*height*(header[16]/8);
    if(!width || !height || bytes<0 || uint64_t(bytes)<expected)return false;
    input.close();

    const DecodedImage image = decodeImageFile(tgaPath);
    if (image.rgba8.empty()) {
        return false;
    }

    if (writePngFromRgba8(pngPath, image.width, image.height, image.rgba8.data())) {
        std::fprintf(stderr, "[Renderer] screenshot saved: %s (%ux%u)\n",
                     pngPath.c_str(), image.width, image.height);
        if(captureBase!=outputBase) {
            std::error_code error;
            std::filesystem::copy_file(tgaPath,screenshotTgaPath(outputBase),std::filesystem::copy_options::overwrite_existing,error);
            if(error)std::fprintf(stderr,"[Renderer] screenshot TGA sidecar copy failed: %s\n",error.message().c_str());
            std::filesystem::remove(tgaPath,error);
        }
        return true;
    }
    return false;
}

std::string ScreenshotCaptureQueue::enqueue(const std::string& outputBase) {
    if(full() || outputBase.empty())return {};
    static std::atomic<uint64_t> serial{uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    auto capture=outputBase+".aycapture-"+std::to_string(serial.fetch_add(1,std::memory_order_relaxed));
    _pending.push_back({capture,outputBase,_frame});return capture;
}
void ScreenshotCaptureQueue::advance(bool finalAttempt) {
    ++_frame;
    while(!_pending.empty()) {
        const auto& request=_pending.front();
        if(finalizeScreenshotSidecar(request.capture,request.output)) {_pending.pop_front();continue;}
        if(!finalAttempt && _frame-request.submitted<120)return;
        std::fprintf(stderr,"[Renderer] screenshot callback incomplete or PNG unwritable: %s\n",request.output.c_str());
        std::error_code error;std::filesystem::remove(screenshotTgaPath(request.capture),error);
        _pending.pop_front();
    }
}

} // namespace ayt::render::detail
