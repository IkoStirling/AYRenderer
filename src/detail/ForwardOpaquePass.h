#pragma once

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/GpuResources.h"

#include <cstdint>
#include <unordered_map>

namespace ayt::render::detail
{

class ForwardOpaquePass {
public:
    static constexpr uint8_t kMainViewId = 0;

    void execute(BGFXAdapter& adapter, shader::ShaderResourcePool& pool,
                 const RenderScene& scene,
                 const std::unordered_map<uint64_t, GpuMesh>& meshes,
                 const std::unordered_map<uint64_t, GpuTexture>& textures,
                 std::unordered_map<uint64_t, GpuMaterial>& materials,
                 uint16_t viewportWidth, uint16_t viewportHeight,
                 const ayt::math::Float4x4& view,
                 const ayt::math::Float4x4& projection);

private:
    static void flushMaterial(GpuMaterial& material,
                              const std::unordered_map<uint64_t, GpuTexture>& textures);
};

} // namespace ayt::render::detail
