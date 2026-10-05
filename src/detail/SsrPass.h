#pragma once
#include "detail/RenderPass.h"
#include "detail/FullscreenPassGeometry.h"
#include "AYShader/ShaderResource.h"
namespace ayt::render::detail {
class SsrPass final : public RenderPass {
public:
    static constexpr uint8_t kViewId = 32;
    std::string_view name() const override { return "SSR"; }
    uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter);
private:
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
};
}
