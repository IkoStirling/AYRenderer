#pragma once

#include "AYMath/MathTypes.h"

#include <algorithm>
#include <cmath>

namespace ayt::render::detail
{

inline float sanitizeDepthHazeStrength(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

inline float sanitizeDepthHazeDensity(float value) noexcept
{
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

inline ayt::math::FVector3 sanitizeDepthHazeColor(
    const ayt::math::FVector3& value) noexcept
{
    const auto clean = [](float component) noexcept {
        return std::isfinite(component) ? std::max(component, 0.0f) : 0.0f;
    };
    return {clean(value.x), clean(value.y), clean(value.z)};
}

inline bool selectDepthHazeStage(bool requested,
                                 float strength,
                                 bool passPresent,
                                 bool passEnabled,
                                 bool gbufferPresent,
                                 bool lightingPresent,
                                 uint16_t width,
                                 uint16_t height) noexcept
{
    return requested
        && sanitizeDepthHazeStrength(strength) > 0.0f
        && passPresent
        && passEnabled
        && gbufferPresent
        && lightingPresent
        && width > 0
        && height > 0;
}

inline float effectiveDepthHazeStrength(float requestedStrength,
                                        bool producedThisFrame,
                                        bool sourceReady) noexcept
{
    return producedThisFrame && sourceReady
        ? sanitizeDepthHazeStrength(requestedStrength)
        : 0.0f;
}

} // namespace ayt::render::detail
