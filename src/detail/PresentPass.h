#pragma once

#include "detail/FullscreenPassGeometry.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYShader/ShaderResource.h"

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Final presentation boundary. It is intentionally a minimal one-texture
// copy: all color transforms belong before this pass, while EditorOverlay and
// UI remain after it on the default backbuffer.
class PresentPass final : public RenderPass {
public:
    static constexpr uint8_t kPresentViewId = 16;

    std::string_view name() const override { return "Present"; }
    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept {
        return _geometry.isReady() && _program.isValid()
            && _tFinalColor != ayt::shader::InvalidBinding;
    }

    void destroyResources(BGFXAdapter& adapter);

private:
    void ensureProgram(shader::ShaderResourcePool& pool);

    FullscreenPassGeometry _geometry;
    ayt::shader::ShaderResource _program;
    ayt::shader::BindingId _tFinalColor = ayt::shader::InvalidBinding;
    uint16_t _programRetryFrames = 0;
};

extern const char* const kPresentCacheKeyCStr;
const char* presentPhoskiaSourceForTests() noexcept;

} // namespace ayt::render::detail
