#pragma once

#include "AYRenderer/RenderTypes.h"

#include <chrono>
#include <cstdint>

namespace ayt::render::detail
{

class DebugOverlay {
public:
    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return _enabled; }

    void setSuppressed(bool suppressed);
    bool isSuppressed() const noexcept { return _suppressed; }

    void onBeginFrame();
    void onEndFrame(uint32_t drawCalls, uint32_t sceneItems,
                    uint16_t viewportX, uint16_t viewportY,
                    uint16_t width, uint16_t height);
    void resetStats();

    const RenderFrameStats& stats() const noexcept { return _stats; }

private:
    void applyDebugMode();

    bool              _enabled     = false;
    bool              _suppressed  = false;
    RenderFrameStats  _stats{};
    float             _smoothedFrameMs = 0.0f;
    std::chrono::steady_clock::time_point _frameStart{};
};

} // namespace ayt::render::detail
