#include "detail/AnalogDisplayPass.h"
#include "AYRenderer/AnalogDisplayShaderSources.h"
#include "detail/PassExecContext.h"
#include "detail/PresentationStage.h"
namespace ayt::render::detail {
uint32_t AnalogDisplayPass::execute(PassExecContext& c) {
    if(!active())return 0;
    PresentationStageTargets targets;
    if(!resolvePresentationStage(c,FgResourceId::DisplayColor,targets))return 0;
    if(!_geometry.ensure(c.adapter))return 0;
    if(!_program.isValid()) {
        if(_retryFrames){--_retryFrames;return 0;}
        _program=c.pool.acquire(analogDisplayPhoskiaSource(),kAnalogDisplayCacheKey);
        if(!_program.isValid()){_retryFrames=120;return 0;}
    }
    const auto texture=_program.getTextureBinding("inputColor");
    const auto viewport=_program.getUniformBinding("displayViewport");
    const auto params=_program.getUniformBinding("displayParams");
    const auto finish=_program.getUniformBinding("displayFinish");
    if(texture==shader::InvalidBinding || viewport==shader::InvalidBinding || params==shader::InvalidBinding
        || finish==shader::InvalidBinding)return 0;
    const float size[4]={float(c.viewportWidth),float(c.viewportHeight),0,0};
    const float values[4]={_settings.scanlineStrength,_settings.maskStrength,_settings.chromaticOffset,_settings.curvature};
    const float finishValues[4]={_settings.vignetteStrength,_settings.grainStrength,float(_settings.noiseSeed),0};
    _program.setTexture(_program.getTextureStage(texture),texture,toShaderTexture(targets.input));
    _program.setUniform(viewport,size,sizeof(size));_program.setUniform(params,values,sizeof(values));
    _program.setUniform(finish,finishValues,sizeof(finishValues));
    configureFullscreenPassView(c.adapter,kViewId,targets.target,0,0,c.viewportWidth,c.viewportHeight);
    _geometry.bind(c.adapter);c.adapter.setStateDepthTestAlways();_program.submit({kViewId,0});
    publishPresentationStage(c,FgResourceId::DisplayColor);return 1;
}
void AnalogDisplayPass::destroyResources(BGFXAdapter& a) {_program.reset();_geometry.destroy(a);_retryFrames=0;}
} // namespace ayt::render::detail
