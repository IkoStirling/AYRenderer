#include "detail/UIPass.h"

#include "AYUIRenderBackend.h"

namespace ayt::render::detail
{

uint32_t UIPass::execute(
    BGFXAdapter& /*adapter*/,
    shader::ShaderResourcePool& /*pool*/,
    const RenderScene& /*scene*/,
    const std::unordered_map<uint64_t, GpuMesh>& /*meshes*/,
    const std::unordered_map<uint64_t, GpuTexture>& /*textures*/,
    std::unordered_map<uint64_t, GpuMaterial>& /*materials*/,
    uint16_t /*viewportX*/, uint16_t /*viewportY*/,
    uint16_t viewportWidth, uint16_t viewportHeight,
    const FrameContext& /*frame*/,
    uint8_t /*viewId*/)
{
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
