#include "detail/ForwardOpaquePass.h"

#include <bgfx/bgfx.h>

namespace ayt::render::detail
{

void ForwardOpaquePass::flushMaterial(GpuMaterial& material,
                                      const std::unordered_map<uint64_t, GpuTexture>& textures)
{
    if (!material.shader.isValid()) {
        return;
    }
    if (material.colorBinding == shader::InvalidBinding) {
        material.colorBinding = material.shader.getUniformBinding("baseColor");
        if (material.colorBinding == shader::InvalidBinding) {
            material.colorBinding = material.shader.getUniformBinding("color");
        }
    }
    if (material.colorBinding != shader::InvalidBinding && material.hasColorOverride) {
        material.shader.setUniform(material.colorBinding, material.colorOverride.ptr(),
                                   sizeof(float) * 4);
    }
    if (material.mat4Binding != shader::InvalidBinding && material.hasMat4Override) {
        material.shader.setUniform(material.mat4Binding, material.mat4Override.ptr(),
                                   sizeof(float) * 16);
    }

    for (const GpuMaterial::TextureSlot& slot : material.textures) {
        if (slot.binding == shader::InvalidBinding || !slot.texture.isValid()) {
            continue;
        }
        const auto texIt = textures.find(slot.texture.id);
        if (texIt == textures.end() || !bgfx::isValid(texIt->second.handle)) {
            continue;
        }
        material.shader.setTexture(0, slot.binding, toShaderTexture(texIt->second.handle));
    }
}

void ForwardOpaquePass::execute(BGFXAdapter& adapter, shader::ShaderResourcePool& /*pool*/,
                                const RenderScene& scene,
                                const std::unordered_map<uint64_t, GpuMesh>& meshes,
                                const std::unordered_map<uint64_t, GpuTexture>& textures,
                                std::unordered_map<uint64_t, GpuMaterial>& materials,
                                uint16_t viewportWidth, uint16_t viewportHeight,
                                const ayt::math::Float4x4& view,
                                const ayt::math::Float4x4& projection)
{
    adapter.setViewRect(kMainViewId, 0, 0, viewportWidth, viewportHeight);
    adapter.setViewTransform(kMainViewId, view.ptr(), projection.ptr());

    const uint64_t defaultState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                                | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                | BGFX_STATE_CULL_CW;

    for (const DrawItem& item : scene.items()) {
        if (!item.mesh.isValid() || !item.material.isValid()) {
            continue;
        }

        const auto meshIt = meshes.find(item.mesh.id);
        const auto matIt  = materials.find(item.material.id);
        if (meshIt == meshes.end() || matIt == materials.end()) {
            continue;
        }

        const GpuMesh& mesh = meshIt->second;
        if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(mesh.indexBuffer)) {
            continue;
        }

        GpuMaterial& material = matIt->second;
        if (!material.shader.isValid()) {
            continue;
        }

        adapter.setTransform(item.world);
        adapter.setVertexBuffer(mesh.vertexBuffer);
        adapter.setIndexBuffer(mesh.indexBuffer, 0, mesh.indexCount);

        flushMaterial(material, textures);

        shader::DrawCallContext ctx;
        ctx.viewId = kMainViewId;
        ctx.state  = defaultState;
        material.shader.submit(ctx);
    }
}

} // namespace ayt::render::detail
