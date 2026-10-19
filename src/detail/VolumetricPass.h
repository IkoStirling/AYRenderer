#pragma once
#include "detail/RenderPass.h"
#include "detail/VolumetricGrid.h"
#include "AYRenderer/RenderTypes.h"
#include "detail/FullscreenPassGeometry.h"
#include "AYShader/ShaderResource.h"
namespace ayt::render::detail {
class VolumetricPass final:public RenderPass {
public:
 static constexpr uint8_t kViewId=33;
 std::string_view name() const override{return "Volumetric";}
 uint32_t execute(PassExecContext& ctx) override;
 void destroyResources(BGFXAdapter& adapter);
 const VolumetricFrameStats& stats() const noexcept {return _stats;}
private:
 uint32_t executePerformance(PassExecContext& ctx);
 bool beginFroxelInjection(PassExecContext& ctx,bgfx::FrameBufferHandle target,const VolumetricGridLayout& grid);
 uint32_t finishFroxelIntegration(PassExecContext& ctx,bgfx::FrameBufferHandle field,bgfx::TextureHandle input,const VolumetricGridLayout& grid);
 static constexpr uint8_t kInjectionViewId=241;
 uint32_t finishIntegration(PassExecContext& ctx,bgfx::FrameBufferHandle integration,bgfx::TextureHandle input);
 static constexpr uint8_t kReconstructViewId=242;
 FullscreenPassGeometry _geometry;
 shader::ShaderResource _program,_reconstruction,_froxelDensity,_froxelIntegration;
 VolumetricFrameStats _stats;
};
}
