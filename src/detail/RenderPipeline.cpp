#include "detail/RenderPipeline.h"

namespace ayt::render::detail
{

void RenderPipeline::addPass(std::unique_ptr<RenderPass> pass)
{
    _passes.push_back(std::move(pass));
}

uint32_t RenderPipeline::executeAll(
    BGFXAdapter& adapter,
    shader::ShaderResourcePool& pool,
    const RenderScene& scene,
    const std::unordered_map<uint64_t, GpuMesh>& meshes,
    const std::unordered_map<uint64_t, GpuTexture>& textures,
    std::unordered_map<uint64_t, GpuMaterial>& materials,
    uint16_t viewportX, uint16_t viewportY,
    uint16_t viewportWidth, uint16_t viewportHeight,
    const FrameContext& frame,
    uint8_t viewId)
{
    uint32_t total = 0;
    for (auto& pass : _passes) {
        if (pass && pass->isEnabled()) {
            total += pass->execute(
                adapter, pool, scene,
                meshes, textures, materials,
                viewportX, viewportY, viewportWidth, viewportHeight,
                frame, viewId);
        }
    }
    return total;
}

} // namespace ayt::render::detail