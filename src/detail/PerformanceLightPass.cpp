#include "detail/PerformanceLightPass.h"
#include "detail/PassExecContext.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/ShadowPass.h"
#include "detail/BgfxMatrix.h"
#include <AYRenderer/PerformanceShaderSources.h>
#include <algorithm>
#include <cmath>

namespace ayt::render::detail {
uint32_t PerformanceLightPass::execute(PassExecContext& ctx) {
    if(!ctx.performanceLights || ctx.performanceLights->lights.empty() || !ctx.resourceBlackboard
        || !ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend())return 0;
    BlackboardGBufferView gb;
    if(!ctx.resourceBlackboard->resolveGBuffer(gb) || !_geometry.ensure(ctx.adapter))return 0;
    if(!_program.isValid())_program=ctx.pool.acquire(kPerformanceSurfaceSource,"performance_surface_v2_aperture");
    if(!_program.isValid())return 0;
    bool valid=true;
    const auto texture=[&](const char* name,bgfx::TextureHandle handle){
        const auto b=_program.getTextureBinding(name);valid&=b!=shader::InvalidBinding;
        if(b!=shader::InvalidBinding)_program.setTexture(_program.getTextureStage(b),b,toShaderTexture(handle));
    };
    const auto uniform=[&](const char* name,const void* data,uint32_t size){
        const auto b=_program.getUniformBinding(name);valid&=b!=shader::InvalidBinding;
        if(b!=shader::InvalidBinding)_program.setUniform(b,data,size);
    };
    const auto vector=[&](const char* name,float x,float y,float z,float w){float v[4]={x,y,z,w};uniform(name,v,16);};
    auto& packet=*ctx.performanceLights;uint32_t draws=0;
    ctx.adapter.setViewMode(8,bgfx::ViewMode::Sequential);
    const auto vp=ctx.frame.projection*ctx.frame.view;
    for(size_t i=0;i<packet.lights.size();++i) {
        const auto& entry=packet.lights[i];const auto& l=entry.light;
        const auto rect=performanceLightRect(entry,vp,ctx.viewportWidth,ctx.viewportHeight);
        if(!rect.visible){++packet.stats.culledLights;continue;}
        if(draws>=packet.budget.surfaceLights){++packet.stats.surfaceBudgetSkipped;continue;}
        // ShaderResource pending bindings are consumed by each submit.
        texture("surfaceAlbedo",gb.albedo);texture("surfaceNormal",gb.normal);
        texture("surfacePosition",gb.worldPosition);texture("surfaceCoverage",gb.material);
        vector("performanceEye",ctx.frame.cameraPosition.x,ctx.frame.cameraPosition.y,ctx.frame.cameraPosition.z,0);
        const auto d=l.spotDirection;
        const auto axis=std::abs(d.y)>.99f?math::FVector3(0,0,1):math::FVector3(0,1,0);
        const auto right=axis.cross(d).normalize();const auto up=d.cross(right).normalize();
        vector("performancePosition",l.position.x,l.position.y,l.position.z,l.range);
        vector("performanceDirection",d.x,d.y,d.z,0);
        vector("performanceColor",l.color.x*l.intensity,l.color.y*l.intensity,l.color.z*l.intensity,0);
        vector("performanceCone",l.coneCosInner,l.coneCosOuter,std::tan(std::acos(l.coneCosOuter)),entry.apertureRadius);
        vector("performanceRight",right.x,right.y,right.z,std::tan(std::acos(l.coneCosInner)));vector("performanceUp",up.x,up.y,up.z,0);
        vector("performancePatternSettings",float(entry.pattern),entry.patternRotation,entry.patternScale,0);
        float matrix[16];toBgfxColumnMajor(math::Float4x4::identity(),matrix);
        float shadowRect[4]={0,0,1,1};float bias=ctx.frame.shadowBias;bool shadowed=false;
        auto shadowTexture=gb.material;const int slot=packet.shadowSlots[i];
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
        vector("performanceShadowSettings",shadowed?1.f:0.f,bias,0,0);texture("shadowAtlas",shadowTexture);
        if(!valid)break;
        _geometry.bind(ctx.adapter);
        ctx.adapter.setDrawScissor(rect.x,rect.y,rect.width,rect.height);
        ctx.adapter.setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,BGFX_STATE_BLEND_ONE));
        shader::DrawCallContext submit;submit.viewId=8;submit.state=0;_program.submit(submit);++draws;
    }
    ctx.adapter.resetDrawScissor();
    packet.stats.surfaceDraws=draws;return draws;
}
} // namespace ayt::render::detail
