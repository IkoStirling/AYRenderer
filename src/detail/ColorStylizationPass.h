#pragma once

#include "AYRenderer/ColorStylizationSettings.h"
#include "AYShader/ShaderResource.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/RenderPass.h"

namespace ayt::render::detail {

class ColorStylizationPass final : public RenderPass {
public:
    static constexpr uint8_t kViewId = 50;
    std::string_view name() const override { return "ColorStylization"; }
    void setSettings(const ColorStylizationSettings& value) noexcept {
        _settings = sanitizeColorStylization(value);
        setEnabled(colorStylizationActive(_settings));
    }
    bool active() const noexcept { return colorStylizationActive(_settings); }
    uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter);
private:
    ColorStylizationSettings _settings;
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
    uint16_t _retryFrames = 0;
};

} // namespace ayt::render::detail
