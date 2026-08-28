#pragma once

#include <algorithm>
#include <cmath>

namespace ayt::render::detail
{

struct BloomStageState {
    bool extract = false;
    bool blur = false;
};

inline float sanitizeBloomStrength(float value) noexcept
{
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

inline float sanitizeBloomThreshold(float value) noexcept
{
    return std::isfinite(value) ? std::max(value, 0.0f) : 1.0f;
}

inline float sanitizeBloomSoftKnee(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.5f;
}

inline BloomStageState selectBloomStages(float strength,
                                         bool hasExtract,
                                         bool extractEnabled,
                                         bool hasBlur,
                                         bool blurEnabled) noexcept
{
    BloomStageState out;
    // BloomBright has no independent consumer in the renderer. Treat the
    // extract/blur pair as one capability so an incomplete custom pipeline
    // neither allocates an orphan target nor advertises a usable bloom source.
    out.blur = sanitizeBloomStrength(strength) > 0.0f
        && hasExtract && extractEnabled
        && hasBlur && blurEnabled;
    out.extract = out.blur;
    return out;
}

inline float effectiveBloomStrength(float requestedStrength,
                                    bool blurProducedThisFrame) noexcept
{
    return blurProducedThisFrame
        ? sanitizeBloomStrength(requestedStrength)
        : 0.0f;
}

} // namespace ayt::render::detail
