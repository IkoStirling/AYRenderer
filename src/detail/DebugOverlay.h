#pragma once

#include "AYRenderer/RenderTypes.h"

#include <bgfx/bgfx.h>

#include <chrono>
#include <cstdint>
#include <array>
#include <vector>

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
                    const std::vector<RenderPassFrameStats>& passStats,
                    uint16_t viewportX, uint16_t viewportY,
                    uint16_t width, uint16_t height);
    void onFrameSubmitted();
    void resetStats();

    const RenderFrameStats& stats() const noexcept { return _stats; }

private:
    void applyDebugMode();
    void pushFrameCadence(float frameMs);
    void sampleBgfxStats();

    bool              _enabled     = false;
    bool              _suppressed  = false;
    RenderFrameStats  _stats{};
    std::chrono::steady_clock::time_point _frameStart{};
    std::chrono::steady_clock::time_point _lastFrameStart{};
    static constexpr size_t kFrameWindow = 240;
    std::array<float, kFrameWindow> _frameTimes{};
    size_t _frameTimeCount = 0;
    size_t _frameTimeCursor = 0;
    // §P5 M5 (2026-08-24) — cached bgfx::Stats pointer set by
    // sampleBgfxStats() (called at the start of onEndFrame). Used by
    // onEndFrame() to read triPrims without a second
    // bgfx::getStats() call (bgfx::getStats() is cheap but the
    // duplicated call was untidy and the cache makes the "stats
    // come from one place per frame" contract explicit). Cleared
    // to nullptr on resetStats() so a stale pointer can't outlive
    // a frame.
    const bgfx::Stats* _lastBgfxStats = nullptr;
};

} // namespace ayt::render::detail
