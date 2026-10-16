#pragma once
#include "detail/RenderPass.h"
#include "detail/FullscreenPassGeometry.h"
#include "AYShader/ShaderResource.h"
namespace ayt::render::detail {
class VolumetricPass final:public RenderPass {
public:
 static constexpr uint8_t kViewId=33;
 std::string_view name() const override{return "Volumetric";}
 uint32_t execute(PassExecContext& ctx) override;
 void destroyResources(BGFXAdapter& adapter);
private:
 uint32_t executePerformance(PassExecContext& ctx);
 FullscreenPassGeometry _geometry;
 shader::ShaderResource _program;
};
}
