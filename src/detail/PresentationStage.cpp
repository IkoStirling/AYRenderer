#include "detail/PresentationStage.h"
#include "detail/PassExecContext.h"
namespace ayt::render::detail {
bool resolvePresentationStage(PassExecContext& c,FgResourceId output,PresentationStageTargets& targets) {
    targets={};
    const auto fail=[&](const char* reason){targets.failure=reason;return false;};
    if(!c.adapter.isInitialized() || c.adapter.isNoopBackend() || !c.viewportWidth || !c.viewportHeight
        || !c.frameGraph)return fail("backend or viewport unavailable");
    if(!c.frameGraph->semanticProducedThisFrame(FgSemantic::PresentSource))return fail("presentation source not produced this frame");
    const auto source=c.frameGraph->resolveSemantic(FgSemantic::PresentSource);
    if(!BGFXAdapter::isValid(source))return fail("presentation framebuffer unavailable");
    const auto input=c.adapter.getFboAttachment(source,0);
    if(!BGFXAdapter::isValid(input))return fail("presentation color unavailable");
    if(c.frameGraph->isDirectPresentationOutput(output)) {
        targets.input=input;
        targets.x=c.viewportX;targets.y=c.viewportY;
        return true;
    }
    const auto target=c.frameGraph->resolve(output);
    if(!BGFXAdapter::isValid(target))return fail("output framebuffer unavailable");
    if(target.idx==source.idx)return fail("presentation/output framebuffer alias");
    const auto outputTexture=c.adapter.getFboAttachment(target,0);
    if(!BGFXAdapter::isValid(outputTexture))return fail("output color unavailable");
    if(outputTexture.idx==input.idx)return fail("presentation/output color alias");
    targets={target,input};return true;
}
void publishPresentationStage(PassExecContext& c,FgResourceId output) {
    if(c.frameGraph->isDirectPresentationOutput(output)) {
        c.frameGraph->markDirectlyPresented(output);
        return;
    }
    c.frameGraph->markProduced(output);c.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,output);
}
} // namespace ayt::render::detail
