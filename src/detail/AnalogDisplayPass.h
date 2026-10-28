#pragma once
#include "AYRenderer/ColorStylizationSettings.h"
#include "AYShader/ShaderResource.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/PresentationPass.h"
namespace ayt::render::detail {
class AnalogDisplayPass final : public PresentationPass {
public:
    AnalogDisplayPass() : PresentationPass(RenderPassSlot::AnalogDisplay) {}
    static constexpr uint8_t kViewId=presentationStageViewId(RenderPassSlot::AnalogDisplay);
    void setSettings(const ColorStylizationSettings& look) noexcept {
        _settings=sanitizeAnalogDisplay(look.display);_settings.enabled &= look.enabled;
        setEnabled(analogDisplayActive(_settings));
    }
    bool active() const noexcept override {return analogDisplayActive(_settings);}
    void applyPresentationLook(const ColorStylizationSettings& look,
        BGFXAdapter&, RenderResourceBlackboard*) override {setSettings(look);}
    uint32_t execute(PassExecContext& context) override;
    void destroyResources(BGFXAdapter& adapter) override;
private:
    AnalogDisplaySettings _settings;
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
    uint16_t _retryFrames=0;
};
} // namespace ayt::render::detail
