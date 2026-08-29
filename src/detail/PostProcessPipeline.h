#pragma once

#include <algorithm>
#include <cmath>

namespace ayt::render::detail
{

inline float sanitizePostProcessExposure(float value) noexcept
{
    constexpr float kNeutralExposure = 1.0f;
    constexpr float kMaxExposure = 64.0f;
    return std::isfinite(value)
        ? std::clamp(value, 0.0f, kMaxExposure)
        : kNeutralExposure;
}

inline float sanitizePostProcessGamma(float value) noexcept
{
    constexpr float kDefaultGamma = 2.2f;
    constexpr float kMinGamma = 0.1f;
    constexpr float kMaxGamma = 8.0f;
    return std::isfinite(value) && value > 0.0f
        ? std::clamp(value, kMinGamma, kMaxGamma)
        : kDefaultGamma;
}

} // namespace ayt::render::detail
