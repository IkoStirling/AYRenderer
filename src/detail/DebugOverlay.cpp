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

void DebugOverlay::onBeginFrame()
{
    _frameStart = std::chrono::steady_clock::now();
}

void DebugOverlay::onEndFrame(uint32_t drawCalls, uint32_t sceneItems,
                              uint16_t width, uint16_t height)
{
    const auto now = std::chrono::steady_clock::now();
    const float frameMs =
        std::chrono::duration<float, std::milli>(now - _frameStart).count();

    _stats.frameTimeMs = frameMs;
    _stats.drawCalls   = drawCalls;
    _stats.sceneItems  = sceneItems;
    ++_stats.frameCount;

    ++_sampleCount;
    _totalFrameMs += static_cast<double>(frameMs);
    _stats.avgFrameTimeMs =
        static_cast<float>(_totalFrameMs / static_cast<double>(_sampleCount));
    if (_stats.avgFrameTimeMs > 0.0f) {
        _stats.fps = 1000.0f / _stats.avgFrameTimeMs;
    } else {
        _stats.fps = 0.0f;
    }

    if (!_enabled) {
        return;
    }

    bgfx::dbgTextClear();

    const char* rendererName = bgfx::getRendererName(bgfx::getRendererType());

    bgfx::dbgTextPrintf(0, 0, 0x0f, "AYRenderer Debug");
    bgfx::dbgTextPrintf(0, 1, 0x0a, "FPS: %.1f  Frame: %.2f ms", _stats.fps, frameMs);
    bgfx::dbgTextPrintf(0, 2, 0x0a, "Draw: %u  Scene: %u", drawCalls, sceneItems);
    bgfx::dbgTextPrintf(0, 3, 0x07, "Backend: %s  %ux%u",
                        rendererName != nullptr ? rendererName : "?",
                        static_cast<unsigned>(width),
                        static_cast<unsigned>(height));

    const bgfx::Stats* bgfxStats = bgfx::getStats();
    if (bgfxStats != nullptr) {
        const uint32_t triPrims = bgfxStats->numPrims[bgfx::Topology::TriList]
                                + bgfxStats->numPrims[bgfx::Topology::TriStrip];
        bgfx::dbgTextPrintf(0, 4, 0x07, "bgfx draws: %u  tris: %u",
                            bgfxStats->numDraw, triPrims);
    }
}

} // namespace ayt::render::detail
