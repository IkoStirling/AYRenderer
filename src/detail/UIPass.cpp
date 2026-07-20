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
    _backend->setFramebufferSize(viewportWidth, viewportHeight);
    _backend->flushBatches();
    return static_cast<uint32_t>(_backend->getDrawCallCount());
}

} // namespace ayt::render::detail
