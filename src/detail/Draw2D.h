#pragma once

#include "AYRenderer/RenderScene.h"
#include "AYShader/ShaderResource.h"

namespace ayt::render::detail
{

inline bool isOverlay2DItem(const DrawItem& item) noexcept
{
    return item.payload != nullptr
        && item.payload->renderDomain == RenderDomain2D::SceneOverlay;
}

inline bool isWorldLit2DItem(const DrawItem& item) noexcept
{
    return item.payload != nullptr
        && item.payload->renderDomain == RenderDomain2D::WorldLit;
}

// Shared per-instance upload for every 2D geometry program. Missing bindings
// remain legal so Overlay and WorldLit shaders can consume different subsets.
inline void upload2DDrawUniforms(shader::ShaderResource& shader,
                                 const DrawPayload2D& payload)
{
    const float srcRect[4] = {
        payload.sourceRectMin.x, payload.sourceRectMin.y,
        payload.sourceRectMax.x, payload.sourceRectMax.y,
    };
    const float tint[4] = {
        payload.tintRGBA.x, payload.tintRGBA.y,
        payload.tintRGBA.z, payload.tintRGBA.w,
    };
    const float flip[4] = {
        static_cast<float>(payload.flip & 0x01u),
        static_cast<float>((payload.flip >> 1) & 0x01u),
        0.0f, 0.0f,
    };
    const float atlasTexel[4] = {
        payload.atlasTexelSize.x, payload.atlasTexelSize.y, 0.0f, 0.0f,
    };
    const float uvMapping[4] = {
        static_cast<float>(static_cast<uint8_t>(payload.uvMapping)),
        0.0f, 0.0f, 0.0f,
    };
    const float samplingQuality[4] = {
        static_cast<float>(static_cast<uint8_t>(payload.samplingQuality)),
        0.0f, 0.0f, 0.0f,
    };

    const shader::BindingId srcRectBinding = shader.getUniformBinding("srcRect");
    if (srcRectBinding != shader::InvalidBinding) {
        shader.setUniform(srcRectBinding, srcRect, sizeof(srcRect));
    }
    const shader::BindingId tintBinding = shader.getUniformBinding("tint");
    if (tintBinding != shader::InvalidBinding) {
        shader.setUniform(tintBinding, tint, sizeof(tint));
    }
    const shader::BindingId flipBinding = shader.getUniformBinding("flip");
    if (flipBinding != shader::InvalidBinding) {
        shader.setUniform(flipBinding, flip, sizeof(flip));
    }
    const shader::BindingId atlasTexelBinding =
        shader.getUniformBinding("atlasTexel");
    if (atlasTexelBinding != shader::InvalidBinding) {
        shader.setUniform(atlasTexelBinding, atlasTexel, sizeof(atlasTexel));
    }
    const shader::BindingId uvMappingBinding =
        shader.getUniformBinding("uvMapping");
    if (uvMappingBinding != shader::InvalidBinding) {
        shader.setUniform(uvMappingBinding, uvMapping, sizeof(uvMapping));
    }
    const shader::BindingId samplingQualityBinding =
        shader.getUniformBinding("samplingQuality");
    if (samplingQualityBinding != shader::InvalidBinding) {
        shader.setUniform(samplingQualityBinding, samplingQuality,
                          sizeof(samplingQuality));
    }
}

} // namespace ayt::render::detail
