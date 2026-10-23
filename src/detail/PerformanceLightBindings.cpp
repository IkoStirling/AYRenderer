#include "detail/PerformanceLightBindings.h"
#include "detail/PassExecContext.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/ShadowPass.h"
#include "detail/SpotShadowProjection.h"
#include "detail/BgfxMatrix.h"
#include <algorithm>
#include <cmath>
namespace ayt::render::detail {
bool bindPerformanceSurfaceLight(PassExecContext& ctx,shader::ShaderResource& program,size_t i,bool receiveShadows){
    if(!ctx.performanceLights||i>=ctx.performanceLights->lights.size()||!ctx.resourceBlackboard)return false;
    BlackboardGBufferView gb;if(!ctx.resourceBlackboard->resolveGBuffer(gb))return false;
    auto& packet=*ctx.performanceLights;const auto& entry=packet.lights[i];const auto& l=entry.light;bool valid=true;
    const auto texture=[&](const char* name,bgfx::TextureHandle handle){const auto b=program.getTextureBinding(name);valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)program.setTexture(program.getTextureStage(b),b,toShaderTexture(handle));};
    const auto uniform=[&](const char* name,const void* data,uint32_t size){const auto b=program.getUniformBinding(name);valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)program.setUniform(b,data,size);};
    const auto vector=[&](const char* name,float x,float y,float z,float w){const float v[4]={x,y,z,w};uniform(name,v,16);};
    auto gobo=gb.albedo;
    if(entry.pattern==LightPattern::Texture){
        const auto found=ctx.textures.find(entry.goboTexture.id);
        if(found==ctx.textures.end()||!BGFXAdapter::isValid(found->second.handle))return false;
        gobo=found->second.handle;
    }
    // ShaderResource pending bindings are consumed by each submit.
    vector("performanceEye",ctx.frame.cameraPosition.x,ctx.frame.cameraPosition.y,ctx.frame.cameraPosition.z,0);
    float contactVp[16];toBgfxColumnMajor(ctx.frame.projection*ctx.frame.view,contactVp);
    uniform("performanceViewProjection",contactVp,64);
    float contactView[16];toBgfxColumnMajor(ctx.frame.view,contactView);uniform("performanceView",contactView,64);
    vector("performanceContact",l.type==LightType::Point?entry.pointContactDistance:0,entry.pointContactThickness,entry.pointContactBias,entry.pointContactStrength);
    texture("contactPosition",gb.worldPosition);texture("contactCoverage",gb.material);
    const auto d=l.spotDirection;
    const auto axis=std::abs(d.y)>.99f?math::FVector3(0,0,1):math::FVector3(0,1,0);
    const auto right=axis.cross(d).normalize();const auto up=d.cross(right).normalize();
    vector("performancePosition",l.position.x,l.position.y,l.position.z,l.range);
    vector("performanceDirection",d.x,d.y,d.z,l.type==LightType::Point?1.f:0.f);
    const float intensity=performanceLightIntensity(entry);
    vector("performanceColor",l.color.x*intensity,l.color.y*intensity,l.color.z*intensity,0);
    vector("performanceCone",l.coneCosInner,l.coneCosOuter,std::tan(std::acos(l.coneCosOuter)),entry.apertureRadius);
    vector("performanceRight",right.x,right.y,right.z,std::tan(std::acos(l.coneCosInner)));vector("performanceUp",up.x,up.y,up.z,0);
    vector("performancePatternSettings",float(entry.pattern),entry.patternRotation,entry.patternScale,0);
    vector("performancePatternOptics",entry.patternFocus,entry.iris,0,0);
    texture("patternGobo",gobo);
    auto ies=gb.albedo;
    if(entry.iesProfile){const auto found=ctx.textures.find(entry.iesTexture.id);if(found==ctx.textures.end()||!BGFXAdapter::isValid(found->second.handle))return false;ies=found->second.handle;}
    vector("performanceLens",entry.focusDistance,float(entry.prismCount),entry.prismAngle,entry.prismRotation);
    vector("performanceIesSettings",entry.iesProfile?1.f:0.f,0,0,0);texture("iesAtlas",ies);
    float matrix[16];toBgfxColumnMajor(math::Float4x4::identity(),matrix);
    float shadowRect[4]={0,0,1,1};float bias=ctx.frame.shadowBias;bool shadowed=false;
    auto shadowTexture=gb.material;const int slot=receiveShadows?packet.shadowSlots[i]:-1;
    if(ctx.shadowPass && ctx.shadowPass->hasSampleableShadow() && slot>=0
        && slot<int(ctx.shadowPass->perLightShadowCount())) {
        const int projection=int(ctx.shadowPass->shadowLightProjectionSlots()[slot*4]);
        if(projection>=0 && projection<int(ctx.shadowPass->shadowProjectionCount())) {
        std::copy_n(ctx.shadowPass->atlasLightViewProjsColumnMajor()+projection*16,16,matrix);
        std::copy_n(ctx.shadowPass->shadowSampleRects()+projection*4,4,shadowRect);
        shadowTexture=ctx.shadowPass->shadowSampleTexture();shadowed=true;
        if(l.shadowBias>0)bias=l.shadowBias;
        }
    }
    uniform("performanceShadow",matrix,64);uniform("performanceShadowRect",shadowRect,16);
    const float texel=shadowed?1.f/float(ctx.shadowPass->shadowSampleMapSize()):0.f;
    vector("performanceShadowSettings",shadowed?1.f:0.f,bias,entry.shadowSoftness,texel);texture("shadowAtlas",shadowTexture);
    const auto shadowProjection=spotShadowProjection(slot>=0?packet.shadowSources.lights[slot].coneCosOuter:l.coneCosOuter,l.range);
    vector("performanceShadowArea",entry.areaShadow&&shadowed?entry.apertureRadius*entry.iris:0.f,shadowProjection.nearZ,shadowProjection.farZ,shadowProjection.tanHalfFov);
    vector("performanceShadowAreaOptions",16,texel,entry.areaShadowBias,32);
    return valid;
}
} // namespace ayt::render::detail
