#pragma once
#include "AYRenderer/ColorStylizationSettings.h"
#include "AYShader/ShaderResource.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/RenderPass.h"
namespace ayt::render::detail {
class StylizedOutlinePass final : public RenderPass {
public:
    static constexpr uint8_t kViewId=51;
    std::string_view name() const override {return "StylizedOutline";}
    void setSettings(const ColorStylizationSettings& value) noexcept {
        _settings=sanitizeColorStylization(value);setEnabled(stylizationOutlineActive(_settings));
    }
    bool active() const noexcept {return stylizationOutlineActive(_settings);}
    uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter);
private:
    ColorStylizationSettings _settings;
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
    uint16_t _retryFrames=0;
};
} // namespace ayt::render::detail
