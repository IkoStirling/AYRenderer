#include "detail/PresentationPass.h"
#include "detail/RenderPipeline.h"
#include "detail/PresentationStage.h"

#include <array>
#include <cassert>
#include <exception>

namespace ayt::render::detail {
namespace {
template<class Visitor>
void visitPresentationPasses(RenderPipeline& pipeline, Visitor&& visitor)
{
    for (auto* pass : pipeline.presentationPasses()) visitor(*pass);
}
}

PresentationPass::PresentationPass(RenderPassSlot slot) : _info(presentationStageInfo(slot))
{
    assert(_info && "PresentationPass requires a registered structural descriptor");
}

bool PresentationPass::resolveTargets(PassExecContext& context, PresentationStageTargets& targets) const
{
    return resolvePresentationStage(context,_info->output,targets);
}

void PresentationPass::publishOutput(PassExecContext& context) const
{
    publishPresentationStage(context,_info->output);
}

PresentationStageFrame PresentationPass::preparePresentationFrame(PresentationFrameContext& context)
{
    PresentationStageFrame result;
    preparePresentationFrame(context,result);
    return result;
}

void PresentationPass::preparePresentationFrame(PresentationFrameContext& context,
    PresentationStageFrame& result)
{
    result.info = _info;
    result.extraReads.clear();
    result.imports.clear();
    result.historyTarget=BGFX_INVALID_HANDLE;
    result.enabled = context.colorChainEnabled && isEnabled() && active();
    if (result.enabled) {
        const auto failed=[&](const char* reason) noexcept {
            // A hook may have appended partial imports before throwing. None
            // of those contributions may reach the shared graph this frame.
            result.enabled=false;result.extraReads.clear();result.imports.clear();
            result.historyTarget=BGFX_INVALID_HANDLE;
            try {
                invalidatePresentationHistory(ResourceInvalidationReason::PreparationFailed,context.blackboard);
            } catch(...) {
                rateLimitedEarlyReturn(_info->name.data(),"preparation history invalidation threw");
            }
            rateLimitedEarlyReturn(_info->name.data(),reason);
        };
        try {preparePresentationResources(context,result);}
        catch(const std::exception& error) {failed(error.what());}
        catch(...) {failed("presentation preparation threw a non-standard exception");}
    }
    else if (!context.colorChainEnabled && isEnabled() && active())
        invalidatePresentationHistory(ResourceInvalidationReason::ProducerFailure, context.blackboard);
    // Hooks contribute frame resources/eligibility, never another pass's
    // structural identity. Failed preparation cannot export partial imports.
    result.info=_info;
    if(!result.enabled) {
        result.extraReads.clear();result.imports.clear();result.historyTarget=BGFX_INVALID_HANDLE;
    }
}

std::vector<PresentationStageFrame> preparePresentationStages(
    RenderPipeline& pipeline, PresentationFrameContext& context)
{
    std::vector<PresentationStageFrame> result;
    preparePresentationStages(pipeline,context,result);
    return result;
}

void preparePresentationStages(RenderPipeline& pipeline, PresentationFrameContext& context,
    std::vector<PresentationStageFrame>& storage)
{
    const auto& passes=pipeline.presentationPasses();
    storage.resize(passes.size());
    for(size_t i=0;i<passes.size();++i)passes[i]->preparePresentationFrame(context,storage[i]);
}

void finishPresentationStages(RenderPipeline& pipeline, RenderResourceBlackboard* blackboard)
{
    visitPresentationPasses(pipeline, [&](auto& pass) { pass.finishPresentationFrame(blackboard); });
}

void configurePresentationLook(RenderPipeline& pipeline, const ColorStylizationSettings& look,
    BGFXAdapter& adapter, RenderResourceBlackboard* blackboard)
{
    visitPresentationPasses(pipeline, [&](auto& pass) { pass.applyPresentationLook(look, adapter, blackboard); });
}

void invalidatePresentationHistories(RenderPipeline& pipeline,
    ResourceInvalidationReason reason, RenderResourceBlackboard* blackboard)
{
    visitPresentationPasses(pipeline, [&](auto& pass) { pass.invalidatePresentationHistory(reason, blackboard); });
}

void destroyPresentationResources(RenderPipeline& pipeline, BGFXAdapter& adapter)
{
    visitPresentationPasses(pipeline, [&](auto& pass) { pass.destroyResources(adapter); });
}

} // namespace ayt::render::detail
