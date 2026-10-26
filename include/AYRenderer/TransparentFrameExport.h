#pragma once

#include <AYRenderer/CameraViews.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ayt::render {

/// Top-left, straight-alpha RGBA8 frame for video encoders and compositors.
/// Timestamp is supplied by the host's monotonic presentation clock.
struct TransparentVideoFrame {
    std::uint16_t width = 0, height = 0;
    std::uint64_t presentationTimeNs = 0;
    std::vector<std::uint8_t> pixels;
};

/// Convert a completed premultiplied CameraOutput readback to an owned,
/// top-left straight-alpha frame. Zero-alpha RGB is zeroed. Invalid size or
/// allocation failure leaves out untouched. Call on a completed readback,
/// never while the GPU request is Pending.
bool convertTransparentCameraFrame(const CameraReadbackFrame& readback,
                                   std::uint64_t presentationTimeNs,
                                   TransparentVideoFrame& out);

/// Save one normalized frame as RGBA PNG. Caller supplies a distinct path for
/// each frame and handles timing/encoding or live compositor transport. Writes
/// through a same-directory temporary file; failure leaves the target intact.
bool writeTransparentVideoFramePng(const TransparentVideoFrame& frame,
                                   const std::string& path);

} // namespace ayt::render
