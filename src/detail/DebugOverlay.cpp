#include "detail/DebugOverlay.h"

#include <bgfx/bgfx.h>

#include <cstdio>

namespace ayt::render::detail
{

void DebugOverlay::applyDebugMode()
{
    bgfx::setDebug(_enabled ? BGFX_DEBUG_TEXT : BGFX_DEBUG_NONE);
}

void DebugOverlay::setEnabled(bool enabled)
{
    if (_enabled == enabled) {
        return;
    }
    _enabled = enabled;
    applyDebugMode();
}

void DebugOverlay::setSuppressed(bool suppressed)
{
    _suppressed = suppressed;
}

void DebugOverlay::onBeginFrame()
{
    _frameStart = std::chrono::steady_clock::now();
}

void DebugOverlay::resetStats()
{
    _stats           = {};
    _smoothedFrameMs = 0.0f;
}

void DebugOverlay::onEndFrame(uint32_t drawCalls, uint32_t sceneItems,
                              uint16_t viewportX, uint16_t viewportY,
                              uint16_t width, uint16_t height)
{
    const auto now = std::chrono::steady_clock::now();
    const float frameMs =
        std::chrono::duration<float, std::milli>(now - _frameStart).count();

    _stats.frameTimeMs = frameMs;
    _stats.drawCalls   = drawCalls;
    _stats.sceneItems  = sceneItems;
    ++_stats.frameCount;

    if (_smoothedFrameMs <= 0.0f) {
        _smoothedFrameMs = frameMs;
    } else {
        constexpr float kAlpha = 0.08f;
        _smoothedFrameMs = kAlpha * frameMs + (1.0f - kAlpha) * _smoothedFrameMs;
    }
    _stats.avgFrameTimeMs = _smoothedFrameMs;
    _stats.fps = _smoothedFrameMs > 0.0f ? 1000.0f / _smoothedFrameMs : 0.0f;

    bgfx::dbgTextClear();

    if (!_enabled || _suppressed || width < 8 || height < 16) {
        return;
    }

    // dbgText is backbuffer-relative; anchor to the viewport top-left.
    constexpr uint16_t kCellW = 8;
    constexpr uint16_t kCellH = 16;
    const uint16_t col = static_cast<uint16_t>(viewportX / kCellW);
    const uint16_t row = static_cast<uint16_t>(viewportY / kCellH);

    uint32_t triPrims = 0;
    const bgfx::Stats* bgfxStats = bgfx::getStats();
    if (bgfxStats != nullptr) {
        triPrims = bgfxStats->numPrims[bgfx::Topology::TriList]
                 + bgfxStats->numPrims[bgfx::Topology::TriStrip];
    }

    bgfx::dbgTextPrintf(col, row + 0, 0x0a, "FPS %.1f  %.1fms", _stats.fps, frameMs);
    bgfx::dbgTextPrintf(col, row + 1, 0x07, "DC %u  SC %u  Tri %u", drawCalls, sceneItems,
                        triPrims);
}

} // namespace ayt::render::detail
