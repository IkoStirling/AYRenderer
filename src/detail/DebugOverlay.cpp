#include "detail/DebugOverlay.h"

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <string_view>
#include <vector>

namespace ayt::render::detail
{

void DebugOverlay::applyDebugMode()
{
    uint32_t flags = BGFX_DEBUG_NONE;
    if (_enabled) flags |= BGFX_DEBUG_TEXT;
    bgfx::setDebug(flags);
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
    const auto now = std::chrono::steady_clock::now();
    if (_lastFrameStart.time_since_epoch().count() != 0) {
        const float cadenceMs = std::chrono::duration<float, std::milli>(
            now - _lastFrameStart).count();
        pushFrameCadence(cadenceMs);
    }
    _lastFrameStart = now;
    _frameStart = now;
}

void DebugOverlay::resetStats()
{
    _stats = {};
    _frameStart = {};
    _lastFrameStart = {};
    _frameTimes.fill(0.0f);
    _frameTimeCount = 0;
    _frameTimeCursor = 0;
    // §P5 M5 (2026-08-24) — clear the cached stats pointer on
    // reset so a stale pointer can't survive a frame boundary.
    _lastBgfxStats = nullptr;
}

void DebugOverlay::pushFrameCadence(float frameMs)
{
    if (!(frameMs > 0.0f)) {
        return;
    }

    _frameTimes[_frameTimeCursor] = frameMs;
    _frameTimeCursor = (_frameTimeCursor + 1) % kFrameWindow;
    _frameTimeCount = std::min(_frameTimeCount + 1, kFrameWindow);

    std::vector<float> sorted(_frameTimes.begin(),
                              _frameTimes.begin() + _frameTimeCount);
    std::sort(sorted.begin(), sorted.end());
    const float sum = std::accumulate(sorted.begin(), sorted.end(), 0.0f);
    const float average = sum / static_cast<float>(_frameTimeCount);
    const auto percentile = [&sorted](float p) {
        const size_t index = std::min(
            sorted.size() - 1,
            static_cast<size_t>(p * static_cast<float>(sorted.size() - 1) + 0.5f));
        return sorted[index];
    };

    _stats.frameTimeMs = frameMs;
    _stats.avgFrameTimeMs = average;
    _stats.p95FrameTimeMs = percentile(0.95f);
    _stats.p99FrameTimeMs = percentile(0.99f);
    _stats.instantaneousFps = 1000.0f / frameMs;
    _stats.fps = average > 0.0f ? 1000.0f / average : 0.0f;
}

namespace {

std::string_view passNameForView(uint16_t view,
                                 const std::vector<RenderPassFrameStats>& passes)
{
    switch (view) {
    case 1: case 2:
    case 18: case 19: case 20: case 21:
    case 22: case 23: case 24: case 25: return "Shadow";
    case 3:          return "MotionVector";
    case 4:          return "ColorGrading";
    case 5: case 245: return "TAA";
    case 6:           return "Skybox";
    case 7:           return "GBuffer";
    case 8:           return "Lighting";
    case 9: case 253: case 254: return "Transparent";
    case 10:          return "BloomExtract";
    case 11: case 12: return "BloomBlur";
    case 13:          return "DepthHaze";
    case 14:          return "SSAO";
    case 15:          return "PostProcess";
    case 16:         return "Present";
    case 17:         return "FXAA";
    case 246:        return "Forward2DOpaque";
    case 247: case 248: case 249: return "SMAA";
    case 251: case 252: return "EditorOverlay";
    case 250:         return "GBufferDebug";
    case 255:         return "UI";
    case 0:
        for (const auto& pass : passes) {
            if (pass.name == "ForwardOpaque") return "ForwardOpaque";
            if (pass.name == "Forward2DOpaque") return "Forward2DOpaque";
        }
        return {};
    default:
        return {};
    }
}

float ticksToMs(int64_t begin, int64_t end, int64_t frequency)
{
    if (frequency <= 0 || end <= begin) {
        return 0.0f;
    }
    return static_cast<float>(static_cast<double>(end - begin) * 1000.0
                              / static_cast<double>(frequency));
}

} // namespace

void DebugOverlay::updateFrameStats(
    uint32_t drawCalls, uint32_t sceneItems,
    const std::vector<RenderPassFrameStats>& passStats,
    const bgfx::Stats* bgfxStats)
{
    _stats.drawCalls = drawCalls;
    _stats.sceneItems = sceneItems;
    _stats.passes = passStats;
    _stats.gpuFrameTimeMs = 0.0f;
    _stats.backendDrawCalls = 0;
    _stats.backendBlitCalls = 0;
    _stats.gpuFrameNumber = 0;
    for (auto& pass : _stats.passes) {
        pass.gpuTimeMs = 0.0f;
    }
    // §P5 M5 (2026-08-24) — cache the pointer so onEndFrame()
    // can read triPrims (and any future stats) without a second
    // bgfx::getStats() call. bgfx::getStats() is cheap but the
    // duplicated call was untidy; the cache makes the "stats
    // come from one place per frame" contract explicit.
    _lastBgfxStats = bgfxStats;
    if (bgfxStats == nullptr) {
        return;
    }

    _stats.backendDrawCalls = bgfxStats->numDraw;
    _stats.backendBlitCalls = bgfxStats->numBlit;
    _stats.gpuFrameNumber = bgfxStats->gpuFrameNum;
    _stats.gpuFrameTimeMs = ticksToMs(bgfxStats->gpuTimeBegin,
                                      bgfxStats->gpuTimeEnd,
                                      bgfxStats->gpuTimerFreq);

    for (uint16_t i = 0; i < bgfxStats->numViews; ++i) {
        const bgfx::ViewStats& view = bgfxStats->viewStats[i];
        const std::string_view passName = passNameForView(view.view, _stats.passes);
        if (passName.empty()) {
            continue;
        }
        const float gpuMs = ticksToMs(view.gpuTimeBegin, view.gpuTimeEnd,
                                      bgfxStats->gpuTimerFreq);
        for (auto& pass : _stats.passes) {
            if (pass.name == passName) {
                pass.gpuTimeMs += gpuMs;
                break;
            }
        }
    }
}

void DebugOverlay::onEndFrame(uint32_t drawCalls, uint32_t sceneItems,
                              const std::vector<RenderPassFrameStats>& passStats,
                              uint16_t viewportX, uint16_t viewportY,
                              uint16_t width, uint16_t height)
{
    // CPU counters describe the current dispatch; GPU counters describe the
    // latest completed backend frame (identified by gpuFrameNumber).
    const bgfx::Stats* gpuSample = bgfx::getStats();
    updateFrameStats(drawCalls, sceneItems, passStats, gpuSample);

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
    // §P5 M5 (2026-08-24) — read from cached pointer set by
    // updateFrameStats() (called from onEndFrame earlier in
    // the frame). Avoids a second bgfx::getStats() call.
    const bgfx::Stats* bgfxStats = _lastBgfxStats;
    if (bgfxStats != nullptr) {
        triPrims = bgfxStats->numPrims[bgfx::Topology::TriList]
                 + bgfxStats->numPrims[bgfx::Topology::TriStrip];
    }

    bgfx::dbgTextPrintf(col, row + 0, 0x0a,
                        "FPS %.1f avg %.2fms p95 %.2f p99 %.2f",
                        _stats.fps, _stats.avgFrameTimeMs,
                        _stats.p95FrameTimeMs, _stats.p99FrameTimeMs);
    bgfx::dbgTextPrintf(col, row + 1, 0x07,
                        "CPU %.2fms GPU %.2fms DC %u/%u B %u SC %u Tri %u",
                        _stats.renderCpuTimeMs, _stats.gpuFrameTimeMs,
                        drawCalls, _stats.backendDrawCalls,
                        _stats.backendBlitCalls, sceneItems, triPrims);

    const uint16_t maxRows = static_cast<uint16_t>(height / kCellH);
    uint16_t passRow = static_cast<uint16_t>(row + 2);
    for (const auto& pass : _stats.passes) {
        if (passRow >= maxRows) break;
        bgfx::dbgTextPrintf(col, passRow++, 0x07, "%-14s dc=%3u cpu=%5.2f gpu=%5.2f",
                            pass.name.c_str(), pass.drawCalls,
                            pass.cpuTimeMs, pass.gpuTimeMs);
    }
}

void DebugOverlay::onFrameSubmitted()
{
    const auto now = std::chrono::steady_clock::now();
    if (_frameStart.time_since_epoch().count() != 0) {
        _stats.renderCpuTimeMs = std::chrono::duration<float, std::milli>(
            now - _frameStart).count();
    }
    ++_stats.frameCount;
}

} // namespace ayt::render::detail
