#pragma once

#include "detail/FullscreenPassGeometry.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYShader/ShaderResource.h"

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Display-referred edge smoothing between PostProcess and Present. The pass
// writes an offscreen FrameGraph target and promotes PresentSource only after
// a successful submit, so any resource/shader failure preserves the unfiltered
// FinalLdrColor presentation path.
class FXAAPass final : public RenderPass {
public:
    static constexpr uint8_t kFxaaViewId = 17;

    std::string_view name() const override { return "FXAA"; }
    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept {
        return _geometry.isReady() && _program.isValid()
            && _tInputColor != ayt::shader::InvalidBinding
            && _uInverseViewport != ayt::shader::InvalidBinding;
    }

    void destroyResources(BGFXAdapter& adapter);

private:
    void ensureProgram(shader::ShaderResourcePool& pool);

    FullscreenPassGeometry _geometry;
    ayt::shader::ShaderResource _program;
    ayt::shader::BindingId _tInputColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uInverseViewport = ayt::shader::InvalidBinding;
    uint16_t _programRetryFrames = 0;
};

extern const char* const kFxaaCacheKeyCStr;
const char* fxaaPhoskiaSourceForTests() noexcept;

} // namespace ayt::render::detail
