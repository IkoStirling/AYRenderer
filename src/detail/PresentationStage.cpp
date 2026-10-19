#include "detail/PresentationStage.h"
#include "detail/PassExecContext.h"
namespace ayt::render::detail {
bool resolvePresentationStage(PassExecContext& c,FgResourceId output,PresentationStageTargets& targets) {
    targets={};
    if(!c.adapter.isInitialized() || c.adapter.isNoopBackend() || !c.viewportWidth || !c.viewportHeight
        || !c.frameGraph || !c.frameGraph->semanticProducedThisFrame(FgSemantic::PresentSource))return false;
    const auto source=c.frameGraph->resolveSemantic(FgSemantic::PresentSource);
    if(!BGFXAdapter::isValid(source))return false;
    const auto input=c.adapter.getFboAttachment(source,0);
    if(!BGFXAdapter::isValid(input))return false;
    const auto target=c.frameGraph->resolve(output);
    if(!BGFXAdapter::isValid(target) || target.idx==source.idx)return false;
    const auto outputTexture=c.adapter.getFboAttachment(target,0);
    if(!BGFXAdapter::isValid(outputTexture) || outputTexture.idx==input.idx)return false;
    targets={target,input};return true;
}
void publishPresentationStage(PassExecContext& c,FgResourceId output) {
    c.frameGraph->markProduced(output);c.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,output);
}
} // namespace ayt::render::detail
