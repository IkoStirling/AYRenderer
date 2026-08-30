#pragma once

#include "detail/FullscreenPassGeometry.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResource.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace ayt::render::detail
{

// Display-referred 2D-strip LUT grading after FXAA and before Present. The
// pass reads the current PresentSource semantic so FXAA failure naturally
// falls back to FinalLdrColor, and promotes the semantic only after submit.
class ColorGradingPass final : public RenderPass {
public:
    // View 4 was the legacy pre-FinalLdr PostProcess id and has no current
    // producer. Reusing it avoids shrinking the retained-UI view range.
    static constexpr uint8_t kColorGradingViewId = 4;
    static constexpr uint16_t kLutSize = 32;

    std::string_view name() const override { return "ColorGrading"; }
    uint32_t execute(PassExecContext& ctx) override;

    void setStrength(float strength) noexcept;
    float strength() const noexcept { return _strength; }
    void setPreset(ColorGradingPreset preset) noexcept;
    ColorGradingPreset preset() const noexcept { return _preset; }

    bool isReady() const noexcept;
    void destroyResources(BGFXAdapter& adapter);

private:
    void ensureProgram(shader::ShaderResourcePool& pool);
    bool ensureLut(BGFXAdapter& adapter);

    FullscreenPassGeometry _geometry;
    ayt::shader::ShaderResource _program;
    bgfx::TextureHandle _lutTexture = BGFX_INVALID_HANDLE;
    ayt::shader::BindingId _tInputColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tColorLut = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uGradingParams = ayt::shader::InvalidBinding;
    float _strength = 0.75f;
    ColorGradingPreset _preset = ColorGradingPreset::Warm;
    bool _lutDirty = true;
    bool _firstDispatchLogged = false;
    uint16_t _programRetryFrames = 0;
};

extern const char* const kColorGradingCacheKeyCStr;
const char* colorGradingPhoskiaSourceForTests() noexcept;
std::vector<uint8_t> generateColorGradingLutRgba8(ColorGradingPreset preset);

} // namespace ayt::render::detail
