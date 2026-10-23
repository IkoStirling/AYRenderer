#include "detail/CloudPass.h"
#include "detail/CloudNoise.h"
#include "detail/CloudRay.h"
#include "AYRenderer/CloudShaderSources.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/SceneColorPipeline.h"
#include "detail/FgResource.h"
namespace ayt::render::detail {
uint32_t CloudPass::execute(PassExecContext& ctx){
 auto& a=ctx.adapter;const auto s=sanitizeCloudSettings(ctx.frame.clouds);
 if(!s.enabled||s.coverage<=0||s.density<=0||s.absorption<=0||!a.isInitialized()||a.isNoopBackend()||!ctx.frameGraph||!ctx.resourceBlackboard||!ctx.viewportWidth||!ctx.viewportHeight)return 0;
 BlackboardGBufferView gb;if(!ctx.resourceBlackboard->resolveGBuffer(gb)||!ctx.resourceBlackboard->findProduced(BlackboardResourceId::LightingColor))return 0;
 auto input=a.getFboAttachment(selectSceneColorSourceFbo(ctx),0);auto target=ctx.frameGraph->resolve(FgResourceId::CloudColor);auto output=a.getFboAttachment(target,0);
 if(!BGFXAdapter::isValid(input)||!BGFXAdapter::isValid(output)||input.idx==output.idx)return 0;
 math::Float4x4 inverse;if(!cloudInverse(ctx.frame.projection*ctx.frame.view,inverse))return 0;
 const auto borrowed=[&](TextureHandle h,int width,int height){auto it=ctx.textures.find(h.id);if(it==ctx.textures.end())return bgfx::TextureHandle{BGFX_INVALID_HANDLE};const auto& t=it->second;if(t.srgb||t.cube||(width&&t.width!=width)||(height&&t.height!=height))return bgfx::TextureHandle{BGFX_INVALID_HANDLE};return t.handle;};
 auto shape=borrowed(s.shapeNoise,cloudAtlasWidth,cloudAtlasHeight),detail=borrowed(s.detailNoise,cloudAtlasWidth,cloudAtlasHeight),weather=borrowed(s.weatherMap,0,0);
 if((s.shapeNoise.isValid()&&!BGFXAdapter::isValid(shape))||(s.detailNoise.isValid()&&!BGFXAdapter::isValid(detail))||(s.weatherMap.isValid()&&!BGFXAdapter::isValid(weather)))return 0;
 if(!s.shapeNoise.isValid()||!s.detailNoise.isValid()){
  if(!BGFXAdapter::isValid(_noise)||_seed!=s.seed){const auto pixels=makeCloudNoise(s.seed);auto next=a.createTexture2D(cloudAtlasWidth,cloudAtlasHeight,pixels.data(),BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP);if(!BGFXAdapter::isValid(next))return 0;if(BGFXAdapter::isValid(_noise))a.destroy(_noise);_noise=next;_seed=s.seed;}
  if(!s.shapeNoise.isValid())shape=_noise;if(!s.detailNoise.isValid())detail=_noise;
 }
 if(!s.weatherMap.isValid()){if(!BGFXAdapter::isValid(_white)){const uint8_t pixels[]={255,255,255,255};_white=a.createTexture2D(1,1,pixels);}weather=_white;}
 if(!BGFXAdapter::isValid(weather)||!_geometry.ensure(a))return 0;if(!_program.isValid())_program=ctx.pool.acquire(kCloudPhoskiaSource,kCloudCacheKey);if(!_program.isValid())return 0;
 bool bound=true;auto tex=[&](const char* n,bgfx::TextureHandle h){auto b=_program.getTextureBinding(n);bound&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_program.setTexture(_program.getTextureStage(b),b,toShaderTexture(h));};
 auto uniform=[&](const char* n,const void* v,uint32_t bytes){auto b=_program.getUniformBinding(n);bound&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_program.setUniform(b,v,bytes);};
 auto v=[&](const char* n,float x,float y,float z,float w){const float p[]={x,y,z,w};uniform(n,p,16);};
 const auto corner=[&](const char* name,float x,float y,float z){float h[4];for(int r=0;r<4;++r)h[r]=inverse.ptr()[r*4]*x+inverse.ptr()[r*4+1]*y+inverse.ptr()[r*4+2]*z+inverse.ptr()[r*4+3];uniform(name,h,16);};
 corner("cloudFar0",-1,1,1);corner("cloudFarX",1,1,1);corner("cloudFarY",-1,-1,1);
 const float nearZ=bgfx::getCaps()->homogeneousDepth?-1.f:0.f;corner("cloudNear0",-1,1,nearZ);corner("cloudNearX",1,1,nearZ);corner("cloudNearY",-1,-1,nearZ);
 v("cloudCamera",ctx.frame.cameraPosition.x,ctx.frame.cameraPosition.y,ctx.frame.cameraPosition.z,std::abs(ctx.frame.projection.row[3].w)>.5f?1.f:0.f);
 v("cloudLayer",s.baseHeight,s.thickness,s.coverage,s.density);v("cloudShape",s.shapeScale,s.detailScale,s.erosion,0);
 v("cloudOptics",s.absorption,s.scattering,s.anisotropy,s.shadowStrength);v("cloudTint",s.tint.x,s.tint.y,s.tint.z,0);
 v("cloudPhase",s.shapePhase.x,s.shapePhase.y,s.shapePhase.z,0);v("cloudDetailPhase",s.detailPhase.x,s.detailPhase.y,s.detailPhase.z,0);
 auto light=Light::directional(ctx.frame.lightDirection,ctx.frame.lightColor);if(ctx.sceneLights){light.intensity=0;for(uint32_t i=0;i<std::min(ctx.sceneLights->count,kMaxSceneLights);++i)if(ctx.sceneLights->lights[i].type==LightType::Directional){light=ctx.sceneLights->lights[i];break;}}
 auto direction=light.direction;auto length=std::sqrt(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);direction=std::isfinite(length)&&length>1e-5f?direction/length:math::FVector3{0,-1,0};
 const auto positive=[](float f){return std::isfinite(f)?std::clamp(f,0.f,10000.f):0.f;};v("cloudLightDirection",direction.x,direction.y,direction.z,0);
 v("cloudLightColor",positive(light.color.x)*positive(light.intensity),positive(light.color.y)*positive(light.intensity),positive(light.color.z)*positive(light.intensity),0);
 v("cloudAmbient",s.ambientColor.x,s.ambientColor.y,s.ambientColor.z,bgfx::getCaps()->homogeneousDepth?-1.f:0.f);
 v("cloudQuality",float(s.quality.steps),float(s.quality.lightSteps),s.quality.maxDistance,float(s.quality.debugView));
 tex("sceneColor",input);tex("worldPosition",gb.worldPosition);tex("geometryCoverage",gb.material);tex("shapeNoise",shape);tex("detailNoise",detail);tex("weatherMap",weather);if(!bound){rateLimitedEarlyReturn("Clouds","missing shader binding");return 0;}
 configureFullscreenPassView(a,kViewId,target,0,0,ctx.viewportWidth,ctx.viewportHeight);_geometry.bind(a);a.setStateDepthTestAlways();shader::DrawCallContext draw;draw.viewId=kViewId;draw.state=0;_program.submit(draw);
 ctx.frameGraph->markProduced(FgResourceId::CloudColor);ctx.resourceBlackboard->publishProduced(BlackboardResourceId::CloudColor,BlackboardResourceLifetime::Transient,target,output,ctx.viewportWidth,ctx.viewportHeight,0);return 1;
}
void CloudPass::destroyResources(BGFXAdapter& a){_program.reset();_geometry.destroy(a);if(BGFXAdapter::isValid(_noise))a.destroy(_noise);if(BGFXAdapter::isValid(_white))a.destroy(_white);_noise=_white=BGFX_INVALID_HANDLE;_seed=0;}
}
