#include "AYRenderer/PbrSurfaceSettings.h"
#include <algorithm>
#include <cmath>
namespace ayt::render
{
PbrSurfaceSettings sanitizePbrSurface(PbrSurfaceSettings value) noexcept
{
    const auto clean = [](float x, float low, float high, float fallback) {
        return std::isfinite(x) ? std::clamp(x, low, high) : fallback;
    };
    value.uvScaleX = clean(value.uvScaleX, .01f, 128, 1);
    value.uvScaleY = clean(value.uvScaleY, .01f, 128, 1);
    value.uvOffsetX = clean(value.uvOffsetX, -1024, 1024, 0);
    value.uvOffsetY = clean(value.uvOffsetY, -1024, 1024, 0);
    value.normalStrength = clean(value.normalStrength, 0, 4, 1);
    value.roughnessBias = clean(value.roughnessBias, -1, 1, 0);
    return value;
}
} // namespace ayt::render
