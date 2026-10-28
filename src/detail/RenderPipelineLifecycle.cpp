#include "detail/RenderPipelineLifecycle.h"
#include "detail/RenderPipeline.h"

namespace ayt::render::detail {
namespace {
void destroyPhase(RenderPipeline& pipeline, BGFXAdapter& adapter,RenderPassCleanupPhase phase)
{
    // Reverse dispatch order within each ownership boundary. Visit every
    // instance, including disabled and duplicate concrete/custom passes.
    const auto& passes=pipeline.passes();
    for(auto it=passes.rbegin();it!=passes.rend();++it)
        if(*it && (*it)->cleanupPhase()==phase)(*it)->destroyResources(adapter);
}
}

void destroyAttachmentBorrowerResources(RenderPipeline& pipeline, BGFXAdapter& adapter)
{
    if (!adapter.isInitialized()) return;
    destroyPhase(pipeline,adapter,RenderPassCleanupPhase::AttachmentBorrower);
}

void destroyMainAttachmentResources(RenderPipeline& pipeline, BGFXAdapter& adapter)
{
    if (!adapter.isInitialized()) return;
    destroyPhase(pipeline,adapter,RenderPassCleanupPhase::AttachmentBorrower);
    destroyPhase(pipeline,adapter,RenderPassCleanupPhase::AttachmentOwner);
}

void destroyPipelineGpuResources(RenderPipeline& pipeline, BGFXAdapter& adapter)
{
    if (!adapter.isInitialized()) return;
    destroyPhase(pipeline,adapter,RenderPassCleanupPhase::AttachmentBorrower);
    destroyPhase(pipeline,adapter,RenderPassCleanupPhase::Independent);
    destroyPhase(pipeline,adapter,RenderPassCleanupPhase::AttachmentOwner);
}
}
