#pragma once
#include "AYRenderer/PreviewScene.h"
#include "detail/PostProcessPipeline.h"
#include <cmath>

namespace ayt::render::detail
{
inline bool validPreviewCamera(const PreviewSceneCamera &camera) noexcept
{
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(camera.view.ptr()[i]) || !std::isfinite(camera.projection.ptr()[i]))
            return false;
    return std::isfinite(camera.position.x) && std::isfinite(camera.position.y) &&
           std::isfinite(camera.position.z);
}
inline PreviewSceneSettings sanitizePreviewSettings(PreviewSceneSettings settings) noexcept
{
    settings.exposure = sanitizePostProcessExposure(settings.exposure);
    settings.gamma = sanitizePostProcessGamma(settings.gamma);
    if (static_cast<uint8_t>(settings.toneMap) > static_cast<uint8_t>(PreviewToneMap::ACES))
        settings.toneMap = PreviewToneMap::None;
    const auto finiteVector = [](const math::FVector3 &v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finiteVector(settings.lightDirection))
        settings.lightDirection = PreviewSceneSettings{}.lightDirection;
    else if (std::max({std::abs(settings.lightDirection.x), std::abs(settings.lightDirection.y),
                       std::abs(settings.lightDirection.z)}) < 1.e-6f)
        settings.lightDirection = PreviewSceneSettings{}.lightDirection;
    if (!finiteVector(settings.lightColor))
        settings.lightColor = PreviewSceneSettings{}.lightColor;
    return settings;
}
} // namespace ayt::render::detail
