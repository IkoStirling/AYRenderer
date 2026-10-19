#include "detail/VolumetricPass.h"
#include "detail/FroxelShaderSources.h"
#include "detail/PassExecContext.h"
#include "detail/BgfxMatrix.h"
#include "detail/FgResource.h"
#include "detail/RenderResourceBlackboard.h"
#include <cmath>
namespace ayt::render::detail {
namespace {
bool bindGridFrame(PassExecContext& ctx,shader::ShaderResource& program,const VolumetricGridLayout& grid,bool density){
 const auto s=sanitizeVolumetricSettings(ctx.frame.volumetric);bool valid=true;
 const auto u=[&](const char* n,const void* data,uint32_t size){const auto b=program.getUniformBinding(n);valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)program.setUniform(b,data,size);};
 const auto v=[&](const char* n,float x,float y,float z,float w){const float data[4]={x,y,z,w};u(n,data,16);};
 float inverse[16];toBgfxColumnMajor((ctx.frame.projection*ctx.frame.view).inverse(),inverse);u("fogInverseViewProjection",inverse,64);
 v("fogCamera",ctx.frame.cameraPosition.x,ctx.frame.cameraPosition.y,ctx.frame.cameraPosition.z,std::abs(ctx.frame.projection.row[3].w)>.5f?1.f:0.f);
 v("fogMedium",s.density,s.scattering,s.anisotropy,s.maxDistance);v("fogCenter",s.center.x,s.center.y,s.center.z,bgfx::getCaps()->homogeneousDepth?-1.f:0.f);
 v("fogExtent",s.halfExtent.x,s.halfExtent.y,s.halfExtent.z,0);v("fogOptions",float(s.steps),float(s.debugView),s.local?1.f:0.f,0);
 v("froxelPacking",float(grid.columns),float(grid.rows),float(grid.slices),0);
 if(density)v("fogNoise",s.noiseStrength,s.noiseScale,s.noiseTime,0);
 else {float view[16];toBgfxColumnMajor(ctx.frame.view,view);u("fogView",view,64);}
 return valid;
}
}
bool VolumetricPass::beginFroxelInjection(PassExecContext& ctx,bgfx::FrameBufferHandle target,const VolumetricGridLayout& grid){
 auto& adapter=ctx.adapter;if(!grid.valid()||!BGFXAdapter::isValid(target))return false;
 if(!_froxelDensity.isValid())_froxelDensity=ctx.pool.acquire(kFroxelDensitySource,kFroxelDensityKey);
 if(!_froxelDensity.isValid()||!bindGridFrame(ctx,_froxelDensity,grid,true))return false;
 configureFullscreenPassView(adapter,kInjectionViewId,target,0,0,grid.atlasWidth,grid.atlasHeight);adapter.setViewMode(kInjectionViewId,bgfx::ViewMode::Sequential);
 adapter.resetDrawScissor();_geometry.bind(adapter);adapter.setStateDepthTestAlways();shader::DrawCallContext submit;submit.viewId=kInjectionViewId;submit.state=0;_froxelDensity.submit(submit);
 return true;
}
uint32_t VolumetricPass::finishFroxelIntegration(PassExecContext& ctx,bgfx::FrameBufferHandle field,bgfx::TextureHandle input,const VolumetricGridLayout& grid){
 auto& adapter=ctx.adapter;BlackboardGBufferView gb;if(!ctx.resourceBlackboard->resolveGBuffer(gb))return 0;
 const auto target=ctx.frameGraph->resolve(FgResourceId::VolumetricIntegration);
 const auto fieldTexture=adapter.getFboAttachment(field,0),output=adapter.getFboAttachment(target,0);
 if(!BGFXAdapter::isValid(fieldTexture)||!BGFXAdapter::isValid(output)||fieldTexture.idx==output.idx)return 0;
 if(!_froxelIntegration.isValid())_froxelIntegration=ctx.pool.acquire(kFroxelIntegrationSource,kFroxelIntegrationKey);
 if(!_froxelIntegration.isValid()||!bindGridFrame(ctx,_froxelIntegration,grid,false))return 0;
 bool valid=true;const auto tex=[&](const char* n,bgfx::TextureHandle t){const auto b=_froxelIntegration.getTextureBinding(n);valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_froxelIntegration.setTexture(_froxelIntegration.getTextureStage(b),b,toShaderTexture(t));};
 tex("froxelField",fieldTexture);tex("worldPosition",gb.worldPosition);tex("geometryCoverage",gb.material);if(!valid)return 0;
 configureFullscreenPassView(adapter,kViewId,target,0,0,grid.width,grid.height);adapter.resetDrawScissor();_geometry.bind(adapter);adapter.setStateDepthTestAlways();shader::DrawCallContext submit;submit.viewId=kViewId;submit.state=0;_froxelIntegration.submit(submit);
 ctx.frameGraph->markProduced(FgResourceId::VolumetricIntegration);
 const auto reconstructed=finishIntegration(ctx,target,input);
 if(reconstructed){_stats.froxel=true;_stats.integrationWidth=grid.width;_stats.integrationHeight=grid.height;_stats.depthSlices=grid.slices;_stats.froxelCells=grid.cells;}
 return 1+reconstructed;
}
}
