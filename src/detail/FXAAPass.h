#pragma once

#include "detail/FullscreenPassGeometry.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYShader/ShaderResource.h"

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Display-referred quality FXAA between PostProcess and Present. The pass
// performs contrast-gated edge classification, a bounded search along the
// detected edge, and a restrained sub-pixel correction. It writes an
// offscreen FrameGraph target and promotes PresentSource only after a
// successful submit, so any resource/shader failure preserves the unfiltered
// FinalLdrColor presentation path.
class FXAAPass final : public RenderPass {
public:
    static constexpr uint8_t kFxaaViewId = 17;

    // Balanced defaults from the FXAA quality range. The lower sub-pixel
    // value is intentional: the previous direction filter blurred texture
    // detail while leaving long stair-step edges visible.
    static constexpr float kEdgeThreshold = 0.125f;
    static constexpr float kEdgeThresholdMin = 0.0312f;
    static constexpr float kSubpixelQuality = 0.50f;
    static constexpr float kSearchThreshold = 0.25f;

    std::string_view name() const override { return "FXAA"; }
    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept {
        return _geometry.isReady() && _program.isValid()
            && _tInputColor != ayt::shader::InvalidBinding
            && _uInverseViewport != ayt::shader::InvalidBinding
            && _uFxaaQuality != ayt::shader::InvalidBinding;
    }

    void destroyResources(BGFXAdapter& adapter);

private:
    void ensureProgram(shader::ShaderResourcePool& pool);

    FullscreenPassGeometry _geometry;
    ayt::shader::ShaderResource _program;
    ayt::shader::BindingId _tInputColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uInverseViewport = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uFxaaQuality = ayt::shader::InvalidBinding;
    uint16_t _programRetryFrames = 0;
};

extern const char* const kFxaaCacheKeyCStr;
const char* fxaaVaryingScForTests() noexcept;
const char* fxaaVertexScForTests() noexcept;
const char* fxaaFragmentScForTests() noexcept;

} // namespace ayt::render::detail
