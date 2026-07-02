#pragma once

#include "AYRenderTypes.h"

#include <chrono>
#include <cstdint>

namespace ayt::render::detail
{

class DebugOverlay {
public:
    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return _enabled; }

    void onBeginFrame();
    void onEndFrame(uint32_t drawCalls, uint32_t sceneItems, uint16_t width, uint16_t height);

    const RenderFrameStats& stats() const noexcept { return _stats; }

private:
    void applyDebugMode();

    bool              _enabled = false;
    RenderFrameStats  _stats{};
    uint64_t          _sampleCount = 0;
    double            _totalFrameMs = 0.0;
    std::chrono::steady_clock::time_point _frameStart{};
};

} // namespace ayt::render::detail
