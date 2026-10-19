#pragma once

#include <string>
#include <deque>
#include <cstdint>

namespace ayt::render::detail
{

std::string screenshotBasePath(const std::string& filePath);
std::string screenshotTgaPath(const std::string& basePath);
std::string screenshotPngPath(const std::string& basePath);

// Convert only a complete callback file; missing/partial files are retried by the queue.
bool finalizeScreenshotSidecar(const std::string& captureBase, const std::string& outputBase);

// Unique callback names prevent an older screenshot from satisfying a newer request.
// FIFO completion preserves ordering when multiple requests target the same output.
class ScreenshotCaptureQueue {
public:
    static constexpr size_t kCapacity=32;
    bool full() const noexcept {return _pending.size()>=kCapacity;}
    size_t size() const noexcept {return _pending.size();}
    std::string enqueue(const std::string& outputBase);
    void advance(bool finalAttempt=false);
private:
    struct Request {std::string capture,output;uint64_t submitted;};
    std::deque<Request> _pending;
    uint64_t _frame=0;
};

} // namespace ayt::render::detail
