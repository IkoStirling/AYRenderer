#include <AYRenderer/TransparentFrameExport.h>

#include <bimg/bimg.h>
#include <bx/file.h>

#include <atomic>
#include <algorithm>
#include <filesystem>
#include <new>
#include <utility>

namespace ayt::render {

bool convertTransparentCameraFrame(const CameraReadbackFrame& readback,
                                   std::uint64_t presentationTimeNs,
                                   TransparentVideoFrame& out) {
    if (!readback.width || !readback.height ||
        readback.width > 2048 || readback.height > 2048 ||
        readback.pixels.size() !=
            std::size_t(readback.width) * readback.height * 4u) return false;
    try {
        TransparentVideoFrame candidate;
        candidate.width = readback.width;
        candidate.height = readback.height;
        candidate.presentationTimeNs = presentationTimeNs;
        candidate.pixels.resize(readback.pixels.size());
        const auto stride = std::size_t(readback.width) * 4u;
        for (std::size_t y = 0; y < readback.height; ++y) {
            const auto sourceY = readback.originBottomLeft
                ? std::size_t(readback.height) - 1u - y : y;
            const auto* source = readback.pixels.data() + sourceY * stride;
            auto* target = candidate.pixels.data() + y * stride;
            for (std::size_t x = 0; x < readback.width; ++x) {
                const auto alpha = source[x * 4u + 3u];
                target[x * 4u + 3u] = alpha;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    target[x * 4u + channel] = alpha
                        ? static_cast<std::uint8_t>(std::min(255u,
                            (std::uint32_t(source[x * 4u + channel]) * 255u +
                             std::uint32_t(alpha) / 2u) / std::uint32_t(alpha)))
                        : 0u;
                }
            }
        }
        out = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool writeTransparentVideoFramePng(const TransparentVideoFrame& frame,
                                   const std::string& path) {
    if (!frame.width || !frame.height || frame.width > 2048 ||
        frame.height > 2048 || path.empty() ||
        frame.pixels.size() != std::size_t(frame.width) * frame.height * 4u)
        return false;
    try {
        const std::filesystem::path target(path);
        if (std::filesystem::exists(target)) return false;
        static std::atomic<std::uint64_t> serial{1};
        const auto temporary = std::filesystem::path(path + ".aytmp-" +
            std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
        bx::FileWriter writer;
        bx::Error error;
        if (!bx::open(&writer, temporary.string().c_str(), false, &error))
            return false;
        const auto written = bimg::imageWritePng(&writer, frame.width,
            frame.height, std::uint32_t(frame.width) * 4u,
            frame.pixels.data(), bimg::TextureFormat::RGBA8, false, &error);
        bx::close(&writer);
        std::error_code fsError;
        if (written <= 0 || error.isOk() == false) {
            std::filesystem::remove(temporary, fsError);
            return false;
        }
        std::filesystem::rename(temporary, target, fsError);
        if (fsError) {
            std::filesystem::remove(temporary, fsError);
            return false;
        }
        return true;
    } catch (const std::filesystem::filesystem_error&) { return false; }
      catch (const std::bad_alloc&) { return false; }
}

} // namespace ayt::render
