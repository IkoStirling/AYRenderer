#pragma once
#include "AYRenderer/ColorStylizationSettings.h"
#include "AYShader/ShaderResource.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/PresentationPass.h"
namespace ayt::render::detail {
class StylizedOutlinePass final : public PresentationPass {
public:
    StylizedOutlinePass() : PresentationPass(RenderPassSlot::StylizedOutline) {}
    static constexpr uint8_t kViewId=presentationStageViewId(RenderPassSlot::StylizedOutline);
    void setSettings(const ColorStylizationSettings& value) noexcept {
        _settings=sanitizeColorStylization(value);setEnabled(stylizationOutlineActive(_settings));
    }
    bool active() const noexcept override {return stylizationOutlineActive(_settings);}
    void applyPresentationLook(const ColorStylizationSettings& look,
        BGFXAdapter&, RenderResourceBlackboard*) override {setSettings(look);}
    uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter) override;
private:
    void preparePresentationResources(PresentationFrameContext&,
        PresentationStageFrame&) override;
    ColorStylizationSettings _settings;
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
    uint16_t _retryFrames=0;
};
} // namespace ayt::render::detail
