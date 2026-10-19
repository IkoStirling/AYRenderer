#include "detail/ColorStylizationPass.h"

#include "AYRenderer/ColorStylizationShaderSources.h"
#include "detail/FgResource.h"
#include "detail/PassExecContext.h"
#include "detail/PresentationStage.h"

namespace ayt::render::detail {

uint32_t ColorStylizationPass::execute(PassExecContext& ctx)
{
    if(!active())return 0;
    PresentationStageTargets targets;
    if(!resolvePresentationStage(ctx,FgResourceId::StylizedColor,targets) || !_geometry.ensure(ctx.adapter))return 0;
    const auto target=targets.target;const auto color=targets.input;
    if (!_program.isValid()) {
        if (_retryFrames) { --_retryFrames; return 0; }
        _program = ctx.pool.acquire(kColorStylizationPhoskiaSource,kColorStylizationCacheKey);
        if (!_program.isValid()) {
            _retryFrames = 120;
            rateLimitedEarlyReturn("ColorStylizationPass", "shader unavailable; retaining presentation source");
        }
    }
    if (!_program.isValid()) return 0;
    const auto texture = _program.getTextureBinding("inputColor");
    const auto params = _program.getUniformBinding("styleParams");
    const auto viewport = _program.getUniformBinding("styleViewport");
    if (texture == shader::InvalidBinding || params == shader::InvalidBinding || viewport == shader::InvalidBinding) return 0;
    for(const char* name:{"mappingParams","paletteColors","printParams","printRotation",
        "shadowColor","highlightColor","inkColor","paperColor"})
        if(_program.getUniformBinding(name)==shader::InvalidBinding)return 0;
    const float settings[4] = {
        _settings.pixelationEnabled ? float(_settings.pixelSize) : 1.0f,
        float(_settings.colorLevels), _settings.ditherStrength,
        _settings.posterizationEnabled ? 1.0f : 0.0f};
    const float size[4] = {float(ctx.viewportWidth),float(ctx.viewportHeight),0,0};
    const float mapping[4] = {float(_settings.colorMapping),float(_settings.paletteSize),0,0};
    const float print[4] = {float(_settings.printPattern),_settings.printSpacing,_settings.printGamma,_settings.printStrength};
    const float angle = _settings.printAngle*0.017453292519943295f;
    const float rotation[4] = {std::cos(angle),std::sin(angle),-std::sin(angle),std::cos(angle)};
    const auto uniform = [&](const char* name,const void* data,uint32_t bytes) {
        const auto binding=_program.getUniformBinding(name);
        if(binding!=shader::InvalidBinding) _program.setUniform(binding,data,bytes);
    };
    const auto colorUniform = [&](const char* name,StylizationColor c) {
        const float rgba[4]={c.r,c.g,c.b,1};uniform(name,rgba,sizeof(rgba));
    };
    float palette[16][4]{};
    for(size_t i=0;i<16;++i) {
        palette[i][0]=_settings.palette[i].r;palette[i][1]=_settings.palette[i].g;
        palette[i][2]=_settings.palette[i].b;palette[i][3]=1;
    }
    configureFullscreenPassView(ctx.adapter,kViewId,target,0,0,ctx.viewportWidth,ctx.viewportHeight);
    _geometry.bind(ctx.adapter);
    _program.setTexture(_program.getTextureStage(texture),texture,toShaderTexture(color));
    _program.setUniform(params,settings,sizeof(settings));
    _program.setUniform(viewport,size,sizeof(size));
    uniform("mappingParams",mapping,sizeof(mapping));
    uniform("paletteColors",palette,sizeof(palette));
    uniform("printParams",print,sizeof(print));
    uniform("printRotation",rotation,sizeof(rotation));
    colorUniform("shadowColor",_settings.shadowColor);
    colorUniform("highlightColor",_settings.highlightColor);
    colorUniform("inkColor",_settings.inkColor);
    colorUniform("paperColor",_settings.paperColor);
    ctx.adapter.setStateDepthTestAlways();
    _program.submit({kViewId,0});
    ctx.frameGraph->markProduced(FgResourceId::StylizedColor);
    ctx.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::StylizedColor);
    return 1;
}

void ColorStylizationPass::destroyResources(BGFXAdapter& adapter)
{
    _program.reset();
    _retryFrames = 0;
    _geometry.destroy(adapter);
}

} // namespace ayt::render::detail
