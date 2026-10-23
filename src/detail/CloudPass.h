#pragma once
#include "detail/RenderPass.h"
#include "detail/FullscreenPassGeometry.h"
namespace ayt::render::detail {
class CloudPass final:public RenderPass {
public:
 static constexpr uint8_t kViewId=54;
 std::string_view name()const override{return "Clouds";}
 uint32_t execute(PassExecContext& ctx)override;
 void destroyResources(BGFXAdapter& adapter);
private:
 FullscreenPassGeometry _geometry;
 shader::ShaderResource _program;
 bgfx::TextureHandle _noise=BGFX_INVALID_HANDLE,_white=BGFX_INVALID_HANDLE;
 uint32_t _seed=0;
};
}
