#include "detail/ColorStylizationPass.h"

#include "AYRenderer/ColorStylizationShaderSources.h"
#include "detail/FgResource.h"
#include "detail/PassExecContext.h"

namespace ayt::render::detail {

uint32_t ColorStylizationPass::execute(PassExecContext& ctx)
{
    if (!active() || !ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()
        || !ctx.viewportWidth || !ctx.viewportHeight || !ctx.frameGraph
        || !ctx.frameGraph->semanticProducedThisFrame(FgSemantic::PresentSource)) return 0;
    const auto source = ctx.frameGraph->resolveSemantic(FgSemantic::PresentSource);
    const auto target = ctx.frameGraph->resolve(FgResourceId::StylizedColor);
    if (!BGFXAdapter::isValid(source) || !BGFXAdapter::isValid(target) || source.idx == target.idx) return 0;
    const auto color = ctx.adapter.getFboAttachment(source, 0);
    if (!BGFXAdapter::isValid(color) || !_geometry.ensure(ctx.adapter)) return 0;
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
    const float settings[4] = {
        _settings.pixelationEnabled ? float(_settings.pixelSize) : 1.0f,
        float(_settings.colorLevels), _settings.ditherStrength,
        _settings.posterizationEnabled ? 1.0f : 0.0f};
    const float size[4] = {float(ctx.viewportWidth),float(ctx.viewportHeight),0,0};
    configureFullscreenPassView(ctx.adapter,kViewId,target,0,0,ctx.viewportWidth,ctx.viewportHeight);
    _geometry.bind(ctx.adapter);
    _program.setTexture(_program.getTextureStage(texture),texture,toShaderTexture(color));
    _program.setUniform(params,settings,sizeof(settings));
    _program.setUniform(viewport,size,sizeof(size));
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
