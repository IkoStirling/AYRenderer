#include "detail/StylizedOutlinePass.h"
#include "AYRenderer/StylizedOutlineShaderSources.h"
#include "detail/FgResource.h"
#include "detail/PassExecContext.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/BgfxMatrix.h"
#include "detail/PresentationStage.h"
namespace ayt::render::detail {
uint32_t StylizedOutlinePass::execute(PassExecContext& ctx) {
    auto& a=ctx.adapter;
    if(!active())return 0;
    PresentationStageTargets targets;
    if(!resolvePresentationStage(ctx,FgResourceId::OutlinedColor,targets))return 0;
    const auto target=targets.target;const auto color=targets.input;
    BlackboardGBufferView gb;
    const bool hasGbuffer=ctx.resourceBlackboard && ctx.resourceBlackboard->resolveGBuffer(gb)
        && gb.width==ctx.viewportWidth && gb.height==ctx.viewportHeight;
    auto depth=hasGbuffer?gb.depth:BGFXAdapter::isValid(ctx.sceneFbo)
        ?a.getFboAttachment(ctx.sceneFbo,1):bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    const bool hasDepth=BGFXAdapter::isValid(depth);
    const bool geometry=_settings.outlineMode>=StylizationOutlineMode::Depth;
    // An unavailable geometry input bypasses the whole effect, never samples a
    // retained stale attachment or silently changes into an image-edge detector.
    if(geometry && !hasDepth)return 0;
    if(!hasDepth)depth=color;
    auto normal=hasGbuffer?gb.normal:color;
    auto inverse=ctx.frame.projection.inverse();
    bool finite=true;for(int i=0;i<16;++i)finite &= std::isfinite(inverse.ptr()[i]);
    if(geometry && !finite)return 0;
    if(!finite)inverse=math::Float4x4::identity();
    if(!_geometry.ensure(a))return 0;
    if(!_program.isValid()) {
        if(_retryFrames){--_retryFrames;return 0;}
        _program=ctx.pool.acquire(kStylizedOutlinePhoskiaSource,kStylizedOutlineCacheKey);
        if(!_program.isValid()){_retryFrames=120;return 0;}
    }
    bool ready=true;
    const auto texture=[&](const char* name,bgfx::TextureHandle h){
        const auto b=_program.getTextureBinding(name);ready &= b!=shader::InvalidBinding;
        if(b!=shader::InvalidBinding)_program.setTexture(_program.getTextureStage(b),b,toShaderTexture(h));
    };
    const auto uniform=[&](const char* name,const void* data,uint32_t bytes){
        const auto b=_program.getUniformBinding(name);ready &= b!=shader::InvalidBinding;
        if(b!=shader::InvalidBinding)_program.setUniform(b,data,bytes);
    };
    const float params[4]={float(_settings.outlineMode),_settings.outlineWidth,0,_settings.outlineStrength};
    const float thresholds[4]={_settings.outlineDepthThreshold,_settings.outlineNormalThreshold,_settings.outlineLuminanceThreshold,0};
    const float available[4]={hasDepth?1.f:0.f,hasGbuffer?1.f:0.f,a.capsHomogeneousDepth()?1.f:0.f,0};
    const auto c=_settings.outlineColor;const float rgba[4]={c.r,c.g,c.b,1};
    const float size[4]={float(ctx.viewportWidth),float(ctx.viewportHeight),0,0};
    float matrix[16];toBgfxColumnMajor(inverse,matrix);
    texture("inputColor",color);texture("outlineDepth",depth);texture("outlineNormal",normal);
    uniform("outlineInverseProjection",matrix,sizeof(matrix));uniform("outlineParams",params,sizeof(params));
    uniform("outlineThresholds",thresholds,sizeof(thresholds));uniform("outlineAvailable",available,sizeof(available));
    uniform("outlineColor",rgba,sizeof(rgba));uniform("styleViewport",size,sizeof(size));
    if(!ready)return 0;
    configureFullscreenPassView(a,kViewId,target,0,0,ctx.viewportWidth,ctx.viewportHeight);
    _geometry.bind(a);a.setStateDepthTestAlways();_program.submit({kViewId,0});
    publishPresentationStage(ctx,FgResourceId::OutlinedColor);return 1;
}
void StylizedOutlinePass::destroyResources(BGFXAdapter& adapter) {
    _program.reset();_geometry.destroy(adapter);_retryFrames=0;
}
} // namespace ayt::render::detail
