#include "detail/UIPass.h"

#include "AYUIRenderBackend.h"

namespace ayt::render::detail
{

uint32_t UIPass::execute(PassExecContext& ctx)
{
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;

    if (_backend == nullptr) {
        return 0;
    }
    if (!_backend->isInitialized()) {
        return 0;
    }
    // AI-1 (2026-07-20): RenderPipeline now owns the UI flush
    // boundary. The host runs UIManager::populateFrame BEFORE
    // Renderer::render (see RendererSubSystem::renderCompositeFrame),
    // which walks the widget tree and accumulates draws on the
    // backend's pendingRects + textBatch. This execute() then:
    //   1) setFramebufferSize — keeps NDC projection in sync with
    //      this pass's declared sub-rect.
    //   2) flushBatches — submits any pending text batches the
    //      widget walk accumulated. Rects are flushed by
    //      UIManager::flushFrame() → backend->endFrame() →
    //      flushColoredRects().
    //   3) Returns the backend's draw-call count for stats.
    //
    // Pre-AI-1 contract (U1+ → U1.5) had UIPass deliberately skip
    // flushBatches and leave the entire flush to the host lambda
    // that drives UIManager::render. That worked but made the
    // RenderPass dispatch path semantically incomplete — UI flush
    // bypassed the Pass taxonomy. AI-1 closes that gap.
    _backend->setFramebufferSize(viewportWidth, viewportHeight);
    _backend->flushBatches();
    return static_cast<uint32_t>(_backend->getDrawCallCount());
}

} // namespace ayt::render::detail
