#include "AYRenderer/TemporalFeedbackSettings.h"
#include <algorithm>
#include <cmath>
namespace ayt::render {
TemporalFeedbackSettings sanitizeTemporalFeedback(TemporalFeedbackSettings s) noexcept {
    if(static_cast<uint8_t>(s.mode)>2)s.mode=TemporalFeedbackMode::Afterimage;
    s.historyWeight=std::isfinite(s.historyWeight)?std::clamp(s.historyWeight,0.f,1.f):0.f;
    s.holdFrames=std::clamp<uint16_t>(s.holdFrames,1,120);s.tileSize=std::clamp<uint16_t>(s.tileSize,4,128);
    s.historyOffsetX=std::isfinite(s.historyOffsetX)?std::clamp(s.historyOffsetX,-64.f,64.f):0;
    s.historyOffsetY=std::isfinite(s.historyOffsetY)?std::clamp(s.historyOffsetY,-64.f,64.f):0;return s;
}
bool temporalFeedbackActive(const TemporalFeedbackSettings& s) noexcept {
    return s.enabled && (s.mode==TemporalFeedbackMode::Afterimage?s.historyWeight>0:s.holdFrames>1);
}
} // namespace ayt::render
