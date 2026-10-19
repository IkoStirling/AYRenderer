#include "detail/VolumetricPass.h"
#include "AYRenderer/VolumetricShaderSources.h"
#include "detail/VolumeReconstructionSource.h"
#include "detail/FroxelShaderSources.h"
#include "detail/PassExecContext.h"
#include "detail/BgfxMatrix.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/SceneColorPipeline.h"
#include "detail/FgResource.h"
#include "detail/ShadowPass.h"
#include <cmath>
namespace ayt::render::detail {
uint32_t VolumetricPass::execute(PassExecContext& ctx) {
 _stats={};auto& adapter=ctx.adapter;const auto s=sanitizeVolumetricSettings(ctx.frame.volumetric);
 if(!s.enabled||s.density<=0||!adapter.isInitialized()||adapter.isNoopBackend()||!ctx.frameGraph||!ctx.resourceBlackboard||!ctx.viewportWidth||!ctx.viewportHeight)return 0;
 if(ctx.performanceLights)return executePerformance(ctx);
 BlackboardGBufferView gb;
 if(!ctx.resourceBlackboard->resolveGBuffer(gb)||!ctx.resourceBlackboard->findProduced(BlackboardResourceId::LightingColor))return 0;
 auto input=adapter.getFboAttachment(selectSceneColorSourceFbo(ctx),0);
 const auto grid=runtimeVolumetricGridLayout(s,ctx.viewportWidth,ctx.viewportHeight);
 const bool froxel=grid.valid(),reduced=froxel||s.downsample>1;
 const uint8_t volumeView=froxel?kInjectionViewId:kViewId;
 const uint16_t width=froxel?grid.atlasWidth:uint16_t((uint32_t(ctx.viewportWidth)+s.downsample-1)/s.downsample),height=froxel?grid.atlasHeight:uint16_t((uint32_t(ctx.viewportHeight)+s.downsample-1)/s.downsample);
 auto target=ctx.frameGraph->resolve(froxel?FgResourceId::FroxelField:reduced?FgResourceId::VolumetricIntegration:FgResourceId::VolumetricColor);
 if(!BGFXAdapter::isValid(input)||!BGFXAdapter::isValid(target))return 0;
 auto output=adapter.getFboAttachment(target,0);
 if(!BGFXAdapter::isValid(output)||input.idx==output.idx)return 0;
 auto inverse=(ctx.frame.projection*ctx.frame.view).inverse();
 for(int i=0;i<16;++i)if(!std::isfinite(inverse.ptr()[i]))return 0;
 if(!_geometry.ensure(adapter))return 0;
 if(!_program.isValid())_program=ctx.pool.acquire(kVolumetricPhoskiaSource,kVolumetricCacheKey);
 if(!_program.isValid())return 0;
 bool bindings=true;
 auto tex=[&](const char* n,bgfx::TextureHandle h){auto b=_program.getTextureBinding(n);bindings&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_program.setTexture(_program.getTextureStage(b),b,toShaderTexture(h));};
 auto u=[&](const char* n,const void* p,uint32_t bytes){auto b=_program.getUniformBinding(n);bindings&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_program.setUniform(b,p,bytes);};
 auto v=[&](const char* n,float x,float y,float z,float w){const float p[4]={x,y,z,w};u(n,p,16);};
 float inv[16],view[16];toBgfxColumnMajor(inverse,inv);toBgfxColumnMajor(ctx.frame.view,view);
 u("fogInverseViewProjection",inv,64);u("fogView",view,64);
 v("fogCamera",ctx.frame.cameraPosition.x,ctx.frame.cameraPosition.y,ctx.frame.cameraPosition.z,std::abs(ctx.frame.projection.row[3].w)>0.5f?1.f:0.f);
 v("fogMedium",s.density,s.scattering,s.anisotropy,s.maxDistance);
 v("fogIntegration",reduced?1.f:0.f,froxel?float(grid.columns):0.f,froxel?float(grid.rows):0.f,froxel?float(grid.slices):0.f);
 v("fogNoise",s.noiseStrength,s.noiseScale,s.noiseTime,0);
 v("fogOptions",float(s.steps),float(s.debugView),!froxel&&s.local?1.f:0.f,froxel?2.f:0.f);
 v("fogCenter",s.center.x,s.center.y,s.center.z,bgfx::getCaps()->homogeneousDepth?-1.f:0.f);
 v("fogExtent",s.halfExtent.x,s.halfExtent.y,s.halfExtent.z,0);v("fogTint",s.tint.x,s.tint.y,s.tint.z,0);
 Light light=Light::directional(ctx.frame.lightDirection,ctx.frame.lightColor);
 if(ctx.sceneLights) {if(s.lightIndex<std::min(ctx.sceneLights->count,kMaxSceneLights))light=ctx.sceneLights->lights[s.lightIndex];else light.intensity=0;}
 auto positive=[](float x){return std::isfinite(x)?std::clamp(x,0.f,10000.f):0.f;};
 light.range=positive(light.range);light.intensity=positive(light.intensity);
 for(int i=0;i<3;++i){light.color[i]=positive(light.color[i]);if(!std::isfinite(light.position[i]))light.position[i]=0;}
 light.coneCosInner=std::isfinite(light.coneCosInner)?std::clamp(light.coneCosInner,-1.f,1.f):1.f;
 light.coneCosOuter=std::isfinite(light.coneCosOuter)?std::clamp(light.coneCosOuter,-1.f,1.f):0.f;
 auto d=light.type==LightType::Spot?light.spotDirection:light.direction;
 const float length=std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);d=length>0.00001f?d/length:math::FVector3(0,-1,0);
 v("fogLightPosition",light.position.x,light.position.y,light.position.z,float(light.type));
 v("fogLightDirection",d.x,d.y,d.z,light.range);
 v("fogLightColor",light.color.x*light.intensity,light.color.y*light.intensity,light.color.z*light.intensity,0);
 v("fogSpot",light.coneCosInner,light.coneCosOuter,0,0);
 v("fogPatternSettings",0,0,1,0);v("fogPatternOptics",0,1,0,0);v("fogPatternRight",1,0,0,0);v("fogPatternUp",0,0,1,0);
 v("fogLightBoundsCenter",0,0,0,0);v("fogLightBoundsExtent",10000,10000,10000,0);
 auto shadow=gb.material;int count=0;float splits[2]={0,0};float bias=ctx.frame.shadowBias;
 const auto identity=math::Float4x4::identity();float matrices[3][16];float rects[3][4]={{0,0,1,1},{0,0,1,1},{0,0,1,1}};
 for(auto& m:matrices)toBgfxColumnMajor(identity,m);
 if(ctx.shadowPass&&ctx.shadowPass->hasSampleableShadow()&&light.type!=LightType::Point&&(light.castShadow||!ctx.sceneLights)) {
  uint32_t index=0;
  if(ctx.sceneLights) {
   const auto order=computeShadowLightOrder(*ctx.sceneLights);
   index=kMaxSceneLights;
   for(uint32_t j=0;j<order.lightCount;++j)if(order.indices[j]==s.lightIndex)index=j;
  }
  if(index>=kMaxSceneLights)return 0;
  const float* slots=ctx.shadowPass->shadowLightProjectionSlots()+index*4;
  count=std::clamp(int(slots[3]),0,3);
  const float* split=ctx.shadowPass->shadowCascadeSplits()+index*4;splits[0]=split[0];splits[1]=split[1];
  for(int c=0;c<count;++c) {int slot=int(slots[c]);if(slot<0||slot>=int(ctx.shadowPass->shadowProjectionCount())){count=0;break;}
   std::copy_n(ctx.shadowPass->atlasLightViewProjsColumnMajor()+slot*16,16,matrices[c]);std::copy_n(ctx.shadowPass->shadowSampleRects()+slot*4,4,rects[c]);
  }
  if(count>0){shadow=ctx.shadowPass->shadowSampleTexture();if(light.shadowBias>0)bias=light.shadowBias;}
 }
 u("fogShadow0",matrices[0],64);u("fogShadow1",matrices[1],64);u("fogShadow2",matrices[2],64);
 u("fogShadowRect0",rects[0],16);u("fogShadowRect1",rects[1],16);u("fogShadowRect2",rects[2],16);
 v("fogShadowParams",splits[0],splits[1],bias,float(count));
 tex("sceneColor",input);tex("worldPosition",gb.worldPosition);tex("geometryCoverage",gb.material);tex("shadowAtlas",shadow);
 v("fogShadowArea",0,.1f,100,1);v("fogShadowAreaOptions",8,0,0,32);
 v("fogLens",0,1,0,0);
 v("fogIesSettings",0,0,0,0);tex("iesAtlas",gb.material);
 tex("patternGobo",gb.material);
 if(!bindings)return 0;
 if(froxel&&!beginFroxelInjection(ctx,target,grid))return 0;
 configureFullscreenPassView(adapter,volumeView,target,0,0,width,height);
 _geometry.bind(adapter);if(froxel)adapter.setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,BGFX_STATE_BLEND_ONE));else adapter.setStateDepthTestAlways();shader::DrawCallContext submit;submit.viewId=volumeView;submit.state=0;_program.submit(submit);
 if(froxel){ctx.frameGraph->markProduced(FgResourceId::FroxelField);return 2+finishFroxelIntegration(ctx,target,input,grid);}
 _stats.integrationWidth=width;_stats.integrationHeight=height;
 if(reduced){ctx.frameGraph->markProduced(FgResourceId::VolumetricIntegration);return 1+finishIntegration(ctx,target,input);}
 ctx.frameGraph->markProduced(FgResourceId::VolumetricColor);
 ctx.resourceBlackboard->publishProduced(BlackboardResourceId::VolumetricColor,BlackboardResourceLifetime::Transient,target,output,ctx.viewportWidth,ctx.viewportHeight,0);
 return 1;
}
uint32_t VolumetricPass::finishIntegration(PassExecContext& ctx,bgfx::FrameBufferHandle integration,bgfx::TextureHandle input){
 auto& adapter=ctx.adapter;const auto s=sanitizeVolumetricSettings(ctx.frame.volumetric);
 BlackboardGBufferView gb;if(!ctx.resourceBlackboard->resolveGBuffer(gb))return 0;
 const auto target=ctx.frameGraph->resolve(FgResourceId::VolumetricColor);
 const auto output=adapter.getFboAttachment(target,0),low=adapter.getFboAttachment(integration,0);
 if(!BGFXAdapter::isValid(output)||!BGFXAdapter::isValid(low)||output.idx==low.idx||output.idx==input.idx)return 0;
 if(!_reconstruction.isValid())_reconstruction=ctx.pool.acquire(kVolumeReconstructionSource,kVolumeReconstructionKey);
 if(!_reconstruction.isValid())return 0;
 bool valid=true;
 const auto u=[&](const char* n,const void* data,uint32_t size){const auto b=_reconstruction.getUniformBinding(n);valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_reconstruction.setUniform(b,data,size);};
 const auto v=[&](const char* n,float x,float y,float z,float w){const float data[4]={x,y,z,w};u(n,data,16);};
 const auto tex=[&](const char* n,bgfx::TextureHandle t){const auto b=_reconstruction.getTextureBinding(n);valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_reconstruction.setTexture(_reconstruction.getTextureStage(b),b,toShaderTexture(t));};
 float inverse[16],view[16];toBgfxColumnMajor((ctx.frame.projection*ctx.frame.view).inverse(),inverse);toBgfxColumnMajor(ctx.frame.view,view);
 u("fogInverseViewProjection",inverse,64);u("fogView",view,64);
 v("fogCamera",ctx.frame.cameraPosition.x,ctx.frame.cameraPosition.y,ctx.frame.cameraPosition.z,std::abs(ctx.frame.projection.row[3].w)>.5f?1.f:0.f);
 v("fogMedium",s.density,s.scattering,s.anisotropy,s.maxDistance);v("fogOptions",float(s.steps),float(s.debugView),s.local?1.f:0.f,0);
 v("fogCenter",s.center.x,s.center.y,s.center.z,bgfx::getCaps()->homogeneousDepth?-1.f:0.f);v("fogExtent",s.halfExtent.x,s.halfExtent.y,s.halfExtent.z,0);v("fogNoise",s.noiseStrength,s.noiseScale,s.noiseTime,0);
 const auto grid=runtimeVolumetricGridLayout(s,ctx.viewportWidth,ctx.viewportHeight);
 const float width=grid.valid()?float(grid.width):float((uint32_t(ctx.viewportWidth)+s.downsample-1)/s.downsample),height=grid.valid()?float(grid.height):float((uint32_t(ctx.viewportHeight)+s.downsample-1)/s.downsample);
 v("reconstructionGrid",width,height,1.f/width,1.f/height);
 tex("sceneColor",input);tex("worldPosition",gb.worldPosition);tex("geometryCoverage",gb.material);tex("fogIntegral",low);
 if(!valid)return 0;
 configureFullscreenPassView(adapter,kReconstructViewId,target,0,0,ctx.viewportWidth,ctx.viewportHeight);
 _geometry.bind(adapter);adapter.setStateDepthTestAlways();shader::DrawCallContext submit;submit.viewId=kReconstructViewId;submit.state=0;_reconstruction.submit(submit);
 ctx.frameGraph->markProduced(FgResourceId::VolumetricColor);ctx.resourceBlackboard->publishProduced(BlackboardResourceId::VolumetricColor,BlackboardResourceLifetime::Transient,target,output,ctx.viewportWidth,ctx.viewportHeight,0);
 return 1;
}
void VolumetricPass::destroyResources(BGFXAdapter& adapter){_program.reset();_reconstruction.reset();_froxelDensity.reset();_froxelIntegration.reset();_stats={};_geometry.destroy(adapter);}
}
