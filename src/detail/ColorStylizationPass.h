#pragma once

#include "AYRenderer/ColorStylizationSettings.h"
#include "AYShader/ShaderResource.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/PresentationPass.h"

namespace ayt::render::detail {

class ColorStylizationPass final : public PresentationPass {
public:
    ColorStylizationPass() : PresentationPass(RenderPassSlot::ColorStylization) {}
    static constexpr uint8_t kViewId = presentationStageViewId(RenderPassSlot::ColorStylization);
    void setSettings(const ColorStylizationSettings& value) noexcept {
        _settings = sanitizeColorStylization(value);
        setEnabled(colorStylizationActive(_settings));
    }
    bool active() const noexcept override { return colorStylizationActive(_settings); }
    void applyPresentationLook(const ColorStylizationSettings& look,
        BGFXAdapter&, RenderResourceBlackboard*) override { setSettings(look); }
    uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter) override;
private:
    ColorStylizationSettings _settings;
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
    uint16_t _retryFrames = 0;
};

} // namespace ayt::render::detail
