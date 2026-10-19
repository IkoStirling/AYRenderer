#include "AYRenderer/AnalogDisplaySettings.h"
#include <algorithm>
#include <cmath>
namespace ayt::render {
AnalogDisplaySettings sanitizeAnalogDisplay(AnalogDisplaySettings s) noexcept {
    const auto clean=[](float x,float upper){return std::isfinite(x)?std::clamp(x,0.f,upper):0.f;};
    s.scanlineStrength=clean(s.scanlineStrength,1);s.maskStrength=clean(s.maskStrength,1);
    s.chromaticOffset=clean(s.chromaticOffset,4);s.curvature=clean(s.curvature,.25f);
    s.vignetteStrength=clean(s.vignetteStrength,1);s.grainStrength=clean(s.grainStrength,.25f);return s;
}
bool analogDisplayActive(const AnalogDisplaySettings& s) noexcept {
    return s.enabled && (s.scanlineStrength>0 || s.maskStrength>0 || s.chromaticOffset>0
        || s.curvature>0 || s.vignetteStrength>0 || s.grainStrength>0);
}
} // namespace ayt::render
