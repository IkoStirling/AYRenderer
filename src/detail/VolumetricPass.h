#pragma once
#include "detail/RenderPass.h"
#include "detail/VolumetricGrid.h"
#include "detail/VolumetricHistoryState.h"
#include "AYRenderer/RenderTypes.h"
#include "detail/FullscreenPassGeometry.h"
#include "AYShader/ShaderResource.h"
namespace ayt::render::detail {
class VolumetricPass final:public RenderPass {
public:
 static constexpr uint8_t kViewId=33;
 std::string_view name() const override{return "Volumetric";}
 uint32_t execute(PassExecContext& ctx) override;
    void destroyResources(BGFXAdapter& adapter) override;
 void prepareHistory(BGFXAdapter& adapter,const VolumetricSettings& settings,const VolumetricGridLayout& grid);
 void invalidateHistory() noexcept {_historyState.invalidate();}
 bgfx::FrameBufferHandle historyRead() const noexcept {return _history[_readIndex];}
 bgfx::FrameBufferHandle historyWrite() const noexcept {return _history[1-_readIndex];}
 const VolumetricFrameStats& stats() const noexcept {return _stats;}
private:
 void destroyHistory(BGFXAdapter& adapter);
 bgfx::TextureHandle resolveHistory(PassExecContext& ctx,bgfx::TextureHandle current,const VolumetricGridLayout& grid);
 static constexpr uint8_t kHistoryViewId=240;
 bgfx::FrameBufferHandle _history[2]={BGFX_INVALID_HANDLE,BGFX_INVALID_HANDLE};
 uint8_t _readIndex=0;
 VolumetricGridLayout _historyGrid;
 VolumetricHistoryState _historyState;
 bool _historySubmitted=false;
 uint32_t executePerformance(PassExecContext& ctx);
 bool beginFroxelInjection(PassExecContext& ctx,bgfx::FrameBufferHandle target,const VolumetricGridLayout& grid);
 uint32_t finishFroxelIntegration(PassExecContext& ctx,bgfx::FrameBufferHandle field,bgfx::TextureHandle input,const VolumetricGridLayout& grid);
 static constexpr uint8_t kInjectionViewId=241;
 uint32_t finishIntegration(PassExecContext& ctx,bgfx::FrameBufferHandle integration,bgfx::TextureHandle input);
 static constexpr uint8_t kReconstructViewId=242;
 FullscreenPassGeometry _geometry;
 shader::ShaderResource _program,_reconstruction,_froxelDensity,_froxelIntegration,_froxelHistory;
 VolumetricFrameStats _stats;
};
}
