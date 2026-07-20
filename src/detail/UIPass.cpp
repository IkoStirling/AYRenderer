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
    // U1+ — RenderPipeline dispatches us, but we do NOT flushBatches
    // here. The active flush is owned by the host lambda that drives
    // UIManager::render (see AYUIManager.cpp:667 — that path calls
    // flushBatches once at the end of each frame). Calling flush here
    // too would be a double-flush against an empty _frame buffer
    // because Renderer::render runs BEFORE the host lambda in
    // RendererSubSystem::renderCompositeFrame. Moving the active
    // flush into this execute() is a U1++ change that requires
    // reordering the host so UIManager::render populates _frame
    // BEFORE Renderer::render fires.
    //
    // What we still do:
    //   1) setFramebufferSize — keeps the backend's projection in
    //      sync with the sub-rect the pipeline is rendering into.
    //   2) return getDrawCallCount — reports the backend's
    //      last-known draw count (one-frame lag; harmless because
    //      lastDrawCalls is a debug stat, not a correctness invariant).
    _backend->setFramebufferSize(viewportWidth, viewportHeight);
    return static_cast<uint32_t>(_backend->getDrawCallCount());
}

} // namespace ayt::render::detail
