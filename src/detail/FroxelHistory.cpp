#include "detail/VolumetricPass.h"
#include "detail/FroxelHistorySource.h"
#include "detail/PassExecContext.h"
#include "detail/FgResource.h"
#include "detail/BgfxMatrix.h"
namespace ayt::render::detail {
void VolumetricPass::destroyHistory(BGFXAdapter& adapter){
 for(auto& fbo:_history){if(BGFXAdapter::isValid(fbo))adapter.destroy(fbo);fbo=BGFX_INVALID_HANDLE;}
 _readIndex=0;_historyGrid={};_historyState.invalidate();_historySubmitted=false;
}
void VolumetricPass::prepareHistory(BGFXAdapter& adapter,const VolumetricSettings& settings,const VolumetricGridLayout& grid){
 if(!settings.froxelHistory||!grid.valid()||!adapter.isInitialized()||adapter.isNoopBackend()){destroyHistory(adapter);return;}
 if(_historyGrid.width==grid.width&&_historyGrid.height==grid.height&&_historyGrid.slices==grid.slices&&BGFXAdapter::isValid(_history[0])&&BGFXAdapter::isValid(_history[1]))return;
 destroyHistory(adapter);
 for(auto& fbo:_history)fbo=adapter.createFrameBuffer(grid.atlasWidth,grid.atlasHeight,bgfx::TextureFormat::RGBA32F,false,true);
 if(!BGFXAdapter::isValid(_history[0])||!BGFXAdapter::isValid(_history[1])){destroyHistory(adapter);return;}
 _historyGrid=grid;
}
bgfx::TextureHandle VolumetricPass::resolveHistory(PassExecContext& ctx,bgfx::TextureHandle current,const VolumetricGridLayout& grid){
 _historySubmitted=false;
 if(!ctx.frame.volumetric.froxelHistory||!BGFXAdapter::isValid(historyRead())||!BGFXAdapter::isValid(historyWrite())){_historyState.invalidate();return current;}
 const auto target=ctx.frameGraph->resolve(FgResourceId::FroxelHistoryWrite);
 const auto previous=ctx.frameGraph->resolve(FgResourceId::FroxelHistoryRead);
 if(!BGFXAdapter::isValid(target)||!BGFXAdapter::isValid(previous)){_historyState.invalidate();return current;}
 const auto old=ctx.adapter.getFboAttachment(previous,0),out=ctx.adapter.getFboAttachment(target,0);
 if(!BGFXAdapter::isValid(old)||!BGFXAdapter::isValid(out)||old.idx==out.idx||current.idx==out.idx){_historyState.invalidate();return current;}
 if(!_froxelHistory.isValid())_froxelHistory=ctx.pool.acquire(kFroxelHistorySource,kFroxelHistoryKey);
 if(!_froxelHistory.isValid()){_historyState.invalidate();return current;}
 const bool reuse=_historyState.reusable(ctx.frame,grid,ctx.adapter.gpuFrameCounter());
 const auto& before=reuse?_historyState.previous:ctx.frame;
 const auto inverse=(ctx.frame.projection*ctx.frame.view).inverse();
 const auto previousVp=before.projection*before.view;
 const float clipNear=bgfx::getCaps()->homogeneousDepth?-1.f:0.f;
 auto previousOrigin=before.cameraPosition;
 if(std::abs(before.projection.row[3].w)>.5f){const auto p=previousVp.inverse()*math::FVector4(0,0,clipNear,1);previousOrigin={p.x/p.w,p.y/p.w,p.z/p.w};}
 bool valid=true;
 const auto u=[&](const char* name,const void* data,uint32_t bytes){const auto b=_froxelHistory.getUniformBinding(name);valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_froxelHistory.setUniform(b,data,bytes);};
 const auto v=[&](const char* name,float x,float y,float z,float w){const float a[4]={x,y,z,w};u(name,a,16);};
 const auto tex=[&](const char* name,bgfx::TextureHandle value){const auto b=_froxelHistory.getTextureBinding(name);valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_froxelHistory.setTexture(_froxelHistory.getTextureStage(b),b,toShaderTexture(value));};
 float matrix[16];toBgfxColumnMajor(inverse,matrix);u("historyInverseViewProjection",matrix,64);toBgfxColumnMajor(previousVp,matrix);u("historyPreviousViewProjection",matrix,64);
 v("historyCamera",ctx.frame.cameraPosition.x,ctx.frame.cameraPosition.y,ctx.frame.cameraPosition.z,std::abs(ctx.frame.projection.row[3].w)>.5f?1.f:0.f);
 v("historyPreviousCamera",previousOrigin.x,previousOrigin.y,previousOrigin.z,0);
 v("historyPreviousDirection",before.view.row[2].x,before.view.row[2].y,before.view.row[2].z,0);
 v("historyPacking",float(grid.columns),float(grid.rows),float(grid.slices),ctx.frame.volumetric.maxDistance);
 v("historyOptions",ctx.frame.volumetric.froxelHistoryWeight,reuse?1.f:0.f,clipNear,0);
 v("historyGrid",1.f/grid.width,1.f/grid.height,0,0);
 tex("currentField",current);tex("previousField",old);
 if(!valid){_historyState.invalidate();return current;}
 configureFullscreenPassView(ctx.adapter,kHistoryViewId,target,0,0,grid.atlasWidth,grid.atlasHeight);ctx.adapter.resetDrawScissor();_geometry.bind(ctx.adapter);ctx.adapter.setStateDepthTestAlways();shader::DrawCallContext submit;submit.viewId=kHistoryViewId;submit.state=0;_froxelHistory.submit(submit);
 ctx.frameGraph->markProduced(FgResourceId::FroxelHistoryWrite);_historySubmitted=true;_stats.historyUsed=reuse;
 return out;
}
}
