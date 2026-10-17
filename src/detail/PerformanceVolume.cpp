#include "detail/VolumetricPass.h"
#include "detail/PassExecContext.h"
#include "detail/BgfxMatrix.h"
#include "detail/FgResource.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/SceneColorPipeline.h"
#include "detail/ShadowPass.h"
#include <AYRenderer/VolumetricShaderSources.h>
#include <algorithm>
#include <cmath>

namespace ayt::render::detail {
uint32_t VolumetricPass::executePerformance(PassExecContext& ctx) {
    auto& adapter=ctx.adapter;auto& packet=*ctx.performanceLights;
    const auto settings=sanitizeVolumetricSettings(ctx.frame.volumetric);
    BlackboardGBufferView gb;
    if(!ctx.resourceBlackboard->resolveGBuffer(gb)||!ctx.resourceBlackboard->findProduced(BlackboardResourceId::LightingColor))return 0;
    const auto input=adapter.getFboAttachment(selectSceneColorSourceFbo(ctx),0);
    const auto target=ctx.frameGraph->resolve(FgResourceId::VolumetricColor);
    if(!BGFXAdapter::isValid(input)||!BGFXAdapter::isValid(target))return 0;
    const auto output=adapter.getFboAttachment(target,0);
    if(!BGFXAdapter::isValid(output)||input.idx==output.idx)return 0;
    const auto inverse=(ctx.frame.projection*ctx.frame.view).inverse();
    for(int i=0;i<16;++i)if(!std::isfinite(inverse.ptr()[i]))return 0;
    if(!_geometry.ensure(adapter))return 0;
    if(!_program.isValid())_program=ctx.pool.acquire(kVolumetricPhoskiaSource,kVolumetricCacheKey);
    if(!_program.isValid())return 0;
    bool valid=true;
    const auto uniform=[&](const char* name,const void* data,uint32_t size){auto b=_program.getUniformBinding(name);
        valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_program.setUniform(b,data,size);};
    const auto vector=[&](const char* name,float x,float y,float z,float w){const float v[4]={x,y,z,w};uniform(name,v,16);};
    const auto texture=[&](const char* name,bgfx::TextureHandle h){auto b=_program.getTextureBinding(name);
        valid&=b!=shader::InvalidBinding;if(b!=shader::InvalidBinding)_program.setTexture(_program.getTextureStage(b),b,toShaderTexture(h));};
    float inv[16],view[16];toBgfxColumnMajor(inverse,inv);toBgfxColumnMajor(ctx.frame.view,view);
    const auto uploadFrame=[&] {
    uniform("fogInverseViewProjection",inv,64);uniform("fogView",view,64);
    vector("fogCamera",ctx.frame.cameraPosition.x,ctx.frame.cameraPosition.y,ctx.frame.cameraPosition.z,std::abs(ctx.frame.projection.row[3].w)>.5f?1.f:0.f);
    vector("fogMedium",settings.density,settings.scattering,settings.anisotropy,settings.maxDistance);
    vector("fogNoise",settings.noiseStrength,settings.noiseScale,settings.noiseTime,0);
    vector("fogCenter",settings.center.x,settings.center.y,settings.center.z,bgfx::getCaps()->homogeneousDepth?-1.f:0.f);
    vector("fogExtent",settings.halfExtent.x,settings.halfExtent.y,settings.halfExtent.z,0);
    vector("fogTint",settings.tint.x,settings.tint.y,settings.tint.z,0);
    texture("sceneColor",input);texture("worldPosition",gb.worldPosition);texture("geometryCoverage",gb.material);
    };
    configureFullscreenPassView(adapter,kViewId,target,0,0,ctx.viewportWidth,ctx.viewportHeight);
    adapter.setViewMode(kViewId,bgfx::ViewMode::Sequential);
    const auto vp=ctx.frame.projection*ctx.frame.view;
    uint32_t draws=0;
    // Extinction is a property of the medium, applied once before additive
    // single-scattering contributions. Repeatedly compositing fog darkens N times.
    for(size_t iteration=0;iteration<=packet.lights.size();++iteration) {
        const bool extinction=iteration==0;
        PerformanceLight empty;empty.light=Light::spot({0,0,0},{0,-1,0},1,0,.95f,.9f,{0,0,0});
        const auto& entry=extinction?empty:packet.lights[iteration-1];const auto& light=entry.light;
        auto rect=LightScreenRect{0,0,ctx.viewportWidth,ctx.viewportHeight,true};
        if(!extinction) {
            if(entry.volumetric<=0||light.intensity<=0)continue;
            rect=performanceLightRect(entry,vp,ctx.viewportWidth,ctx.viewportHeight);if(!rect.visible)continue;
            if(packet.stats.volumeDraws>=packet.budget.volumeLights){++packet.stats.volumeBudgetSkipped;continue;}
        }
        auto gobo=gb.material;
        if(entry.pattern==LightPattern::Texture){
            const auto found=ctx.textures.find(entry.goboTexture.id);
            if(found==ctx.textures.end()||!BGFXAdapter::isValid(found->second.handle))continue;
            gobo=found->second.handle;
        }
        uploadFrame();
        vector("fogOptions",float(settings.steps),float(settings.debugView),settings.local?1.f:0.f,extinction?1.f:2.f);
        vector("fogLightPosition",light.position.x,light.position.y,light.position.z,float(light.type));
        const auto direction=light.spotDirection;
        vector("fogLightDirection",direction.x,direction.y,direction.z,light.range);
        const float intensity=light.intensity*entry.volumetric*performanceLightFluxScale(entry);
        vector("fogLightColor",light.color.x*intensity,light.color.y*intensity,light.color.z*intensity,0);
        vector("fogSpot",light.coneCosInner,light.coneCosOuter,std::tan(std::acos(light.coneCosOuter)),entry.apertureRadius);
        const auto axis=std::abs(direction.y)>.99f?math::FVector3(0,0,1):math::FVector3(0,1,0);
        const auto right=axis.cross(direction).normalize(),up=direction.cross(right).normalize();
        vector("fogPatternRight",right.x,right.y,right.z,std::tan(std::acos(light.coneCosInner)));vector("fogPatternUp",up.x,up.y,up.z,0);
        vector("fogPatternSettings",float(entry.pattern),entry.patternRotation,entry.patternScale,0);
        const float shadowTexel=ctx.shadowPass?1.f/float(ctx.shadowPass->shadowSampleMapSize()):0.f;
        vector("fogPatternOptics",entry.patternFocus,entry.iris,entry.shadowSoftness,shadowTexel);
        texture("patternGobo",gobo);
        math::FVector3 center,extent;performanceLightBounds(entry,center,extent);
        vector("fogLightBoundsCenter",center.x,center.y,center.z,0);vector("fogLightBoundsExtent",extent.x,extent.y,extent.z,0);
        float matrix[16];toBgfxColumnMajor(math::Float4x4::identity(),matrix);float sr[4]={0,0,1,1};
        bool shadowed=false;auto shadow=gb.material;float bias=ctx.frame.shadowBias;
        const int slot=extinction?-1:packet.shadowSlots[iteration-1];
        if(ctx.shadowPass&&ctx.shadowPass->hasSampleableShadow()&&slot>=0&&slot<int(ctx.shadowPass->perLightShadowCount())) {
            const int projection=int(ctx.shadowPass->shadowLightProjectionSlots()[slot*4]);
            if(projection>=0&&projection<int(ctx.shadowPass->shadowProjectionCount())) {
                std::copy_n(ctx.shadowPass->atlasLightViewProjsColumnMajor()+projection*16,16,matrix);
                std::copy_n(ctx.shadowPass->shadowSampleRects()+projection*4,4,sr);
                shadow=ctx.shadowPass->shadowSampleTexture();shadowed=true;if(light.shadowBias>0)bias=light.shadowBias;
            }
        }
        for(const auto* name:{"fogShadow0","fogShadow1","fogShadow2"})uniform(name,matrix,64);
        for(const auto* name:{"fogShadowRect0","fogShadowRect1","fogShadowRect2"})uniform(name,sr,16);
        vector("fogShadowParams",0,0,bias,shadowed?1.f:0.f);texture("shadowAtlas",shadow);
        if(!valid){adapter.resetDrawScissor();return draws;}
        _geometry.bind(adapter);adapter.setDrawScissor(rect.x,rect.y,rect.width,rect.height);
        if(extinction)adapter.setStateDepthTestAlways();
        else adapter.setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,BGFX_STATE_BLEND_ONE));
        shader::DrawCallContext submit;submit.viewId=kViewId;submit.state=0;_program.submit(submit);++draws;
        if(!extinction)++packet.stats.volumeDraws;
        if(settings.debugView==1)break; // Transmittance has no light-dependent term.
    }
    adapter.resetDrawScissor();
    ctx.frameGraph->markProduced(FgResourceId::VolumetricColor);
    ctx.resourceBlackboard->publishProduced(BlackboardResourceId::VolumetricColor,BlackboardResourceLifetime::Transient,target,output,ctx.viewportWidth,ctx.viewportHeight,0);
    return draws;
}
} // namespace ayt::render::detail
