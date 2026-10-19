#include "detail/PerformanceLightPass.h"
#include "detail/PerformanceLightBindings.h"
#include "detail/PassExecContext.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/ShadowPass.h"
#include "detail/SpotShadowProjection.h"
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
    if(!_program.isValid())_program=ctx.pool.acquire(kPerformanceSurfaceSource,"performance_surface_v9_area_shadow");
    if(!_program.isValid())return 0;
    bool valid=true;
    const auto texture=[&](const char* name,bgfx::TextureHandle handle){
        const auto b=_program.getTextureBinding(name);valid&=b!=shader::InvalidBinding;
        if(b!=shader::InvalidBinding)_program.setTexture(_program.getTextureStage(b),b,toShaderTexture(handle));
    };
    auto& packet=*ctx.performanceLights;uint32_t draws=0;
    ctx.adapter.setViewMode(8,bgfx::ViewMode::Sequential);
    const auto vp=ctx.frame.projection*ctx.frame.view;
    for(size_t i=0;i<packet.lights.size();++i) {
        const auto& entry=packet.lights[i];
        const auto rect=performanceLightRect(entry,vp,ctx.viewportWidth,ctx.viewportHeight);
        if(!rect.visible){++packet.stats.culledLights;continue;}
        if(draws>=packet.budget.surfaceLights){++packet.stats.surfaceBudgetSkipped;continue;}
        texture("surfaceAlbedo",gb.albedo);texture("surfaceNormal",gb.normal);
        texture("surfacePosition",gb.worldPosition);texture("surfaceCoverage",gb.material);
        valid=valid&&bindPerformanceSurfaceLight(ctx,_program,i);
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
