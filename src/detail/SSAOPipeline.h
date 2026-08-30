#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ayt::render::detail
{

inline float sanitizeSsaoStrength(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

inline float sanitizeSsaoRadius(float value) noexcept
{
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

inline float sanitizeSsaoBias(float value) noexcept
{
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

inline bool ssaoParametersActive(bool enabled,
                                 float strength,
                                 float radius) noexcept
{
    return enabled
        && sanitizeSsaoStrength(strength) > 0.0f
        && sanitizeSsaoRadius(radius) > 0.0f;
}

inline bool selectSsaoStage(bool enabled,
                            float strength,
                            float radius,
                            bool passPresent,
                            bool passEnabled,
                            bool gbufferPresent,
                            bool gbufferEnabled,
                            bool lightingPresent,
                            bool lightingEnabled,
                            uint16_t viewportWidth,
                            uint16_t viewportHeight) noexcept
{
    return ssaoParametersActive(enabled, strength, radius)
        && passPresent
        && passEnabled
        && gbufferPresent
        && gbufferEnabled
        && lightingPresent
        && lightingEnabled
        && viewportWidth > 0
        && viewportHeight > 0;
}

} // namespace ayt::render::detail
