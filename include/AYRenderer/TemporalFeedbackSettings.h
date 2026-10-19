#pragma once
#include <cstdint>
namespace ayt::render {
enum class TemporalFeedbackMode : uint8_t { Afterimage, FrameHold, TileHold };
/// Stylized image history, independent of simulation/present cadence and TAA.
/// Counts successful main-view submissions. TileHold refreshes every tile once
/// per holdFrames cycle, with deterministic spatial phases, not unbounded staleness.
struct TemporalFeedbackSettings {
    bool enabled=false;
    TemporalFeedbackMode mode=TemporalFeedbackMode::Afterimage;
    float historyWeight=.85f; // 0..1; Afterimage only, 0 is an identity bypass.
    uint16_t holdFrames=3; // 1..120; hold modes, 1 is an identity bypass.
    uint16_t tileSize=24; // 4..128 physical viewport pixels.
    float historyOffsetX=0,historyOffsetY=0; // -64..64 sample displacement in pixels.
    uint16_t noiseSeed=0;
};
TemporalFeedbackSettings sanitizeTemporalFeedback(TemporalFeedbackSettings value) noexcept;
bool temporalFeedbackActive(const TemporalFeedbackSettings& value) noexcept;
} // namespace ayt::render
