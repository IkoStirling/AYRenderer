#pragma once
#include "AYRenderer/ColorStylizationSettings.h"
#include "AYShader/ShaderResource.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/RenderPass.h"
namespace ayt::render::detail {
class AnalogDisplayPass final : public RenderPass {
public:
    static constexpr uint8_t kViewId=52;
    std::string_view name() const override {return "AnalogDisplay";}
    void setSettings(const ColorStylizationSettings& look) noexcept {
        _settings=sanitizeAnalogDisplay(look.display);_settings.enabled &= look.enabled;
        setEnabled(analogDisplayActive(_settings));
    }
    bool active() const noexcept {return analogDisplayActive(_settings);}
    uint32_t execute(PassExecContext& context) override;
    void destroyResources(BGFXAdapter& adapter);
private:
    AnalogDisplaySettings _settings;
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
    uint16_t _retryFrames=0;
};
} // namespace ayt::render::detail
