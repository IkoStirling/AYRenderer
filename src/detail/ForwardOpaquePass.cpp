#include "detail/ForwardOpaquePass.h"

#include "detail/FrameContext.h"

#include <bgfx/bgfx.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ayt::render::detail
{

void ForwardOpaquePass::flushMaterial(GpuMaterial& material,
                                      const std::unordered_map<uint64_t, GpuTexture>& textures,
                                      const FrameContext& frame,
                                      const ayt::math::Float4x4& world)
{
    if (!material.shader.isValid()) {
        return;
    }

    // Phase 1 RD-04: lazy-resolve the `Skeleton` UBO binding for
    // skinned materials. ForwardOpaquePass::execute uses this to
    // upload bone matrices per-draw.
    if (material.boneBlockBinding == shader::InvalidBinding) {
        material.boneBlockBinding =
            material.shader.getUniformBlockBinding("Skeleton");
    }

    const ayt::math::Float4x4 modelViewProj = frame.projection * frame.view * world;
    trySetUniformMat4(material.shader, "u_modelViewProj", "modelViewProj", modelViewProj);

    trySetUniformVec3(material.shader, "cameraPos", frame.cameraPosition.ptr());

    const ayt::math::FVector3 toLight(
        -frame.lightDirection.x, -frame.lightDirection.y, -frame.lightDirection.z);
    const ayt::math::FVector3 toLightDir = toLight.normalize();
    trySetUniformVec3(material.shader, "lightDir", toLightDir.ptr());
    trySetUniformVec3(material.shader, "lightDirection", toLightDir.ptr());
    trySetUniformVec3(material.shader, "lightColor", frame.lightColor.ptr());

    // U1++ — color-uniform upload lifted to RenderPass helper; see
    // RenderPass.cpp::resolveAndApplyColorUniforms. Identical bytes
    // to the prior inline body; TransparentPass uses the same helper.
    RenderPass::resolveAndApplyColorUniforms(material);
    if (material.mat4Binding != shader::InvalidBinding && material.hasMat4Override) {
        if (material.shader.hasUniformBinding(material.mat4Binding)) {
            material.shader.setUniform(material.mat4Binding, material.mat4Override.ptr(),
                                       sizeof(float) * 16);
        } else {
            material.mat4Binding = shader::InvalidBinding;
        }
    }

    for (const GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name.empty() || slot.size == 0) {
            continue;
        }
        const shader::BindingId binding = material.shader.getUniformBinding(slot.name);
        if (binding == shader::InvalidBinding) {
            continue;
        }
        material.shader.setUniform(binding, slot.data, slot.size);
    }

    for (const GpuMaterial::TextureSlot& slot : material.textures) {
        if (slot.name.empty() || !slot.texture.isValid()) {
            continue;
        }
        const shader::BindingId binding = material.shader.getTextureBinding(slot.name);
        if (binding == shader::InvalidBinding) {
            continue;
        }
        const auto texIt = textures.find(slot.texture.id);
        if (texIt == textures.end() || !bgfx::isValid(texIt->second.handle)) {
            continue;
        }
        material.shader.setTexture(0, slot.binding, toShaderTexture(texIt->second.handle));
    }
}

uint32_t ForwardOpaquePass::execute(BGFXAdapter& adapter, shader::ShaderResourcePool& /*pool*/,
                                  const RenderScene& scene,
                                  const std::unordered_map<uint64_t, GpuMesh>& meshes,
                                  const std::unordered_map<uint64_t, GpuTexture>& textures,
                                  std::unordered_map<uint64_t, GpuMaterial>& materials,
                                  uint16_t viewportX, uint16_t viewportY,
                                  uint16_t viewportWidth, uint16_t viewportHeight,
                                  const FrameContext& frame,
                                  uint8_t viewId)
{
    adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, frame.view.ptr(), frame.projection.ptr());

    const uint64_t defaultState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                                | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                                | BGFX_STATE_CULL_CW;

    uint32_t drawCount = 0;

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

        flushMaterial(material, textures, frame, item.world);

        // Phase 1 RD-04: upload per-frame bone matrices to the
        // material's `Skeleton` UBO or top-level `bones[]` uniform.
        if (item.boneMatrices != nullptr && item.jointCount > 0
            && material.shader.isValid()) {
            const size_t byteCount = static_cast<size_t>(item.jointCount) * 64;
            if (byteCount <= 1024) {
                float stackBuf[1024 / sizeof(float)];
                for (uint32_t k = 0; k < item.jointCount; ++k) {
                    std::memcpy(&stackBuf[k * 16],
                                item.boneMatrices[k].ptr(),
                                sizeof(float) * 16);
                }
                bool uploaded = false;
                if (material.boneBlockBinding != shader::InvalidBinding) {
                    material.shader.setUniformBlock(material.boneBlockBinding,
                                                    stackBuf, byteCount);
                    uploaded = true;
                } else {
                    const shader::BindingId bonesUniform =
                        material.shader.getUniformBinding("bones");
                    if (bonesUniform != shader::InvalidBinding) {
                        material.shader.setUniform(bonesUniform, stackBuf, byteCount);
                        uploaded = true;
                    }
                }
                if (!uploaded) {
                    static uint32_t s_missingBoneBindingLog = 0;
                    if (s_missingBoneBindingLog < 3) {
                        std::fprintf(stderr,
                                     "[ForwardOpaquePass] skinned draw skipped bone upload "
                                     "(Skeleton UBO / bones[] binding missing)\n");
                        ++s_missingBoneBindingLog;
                    }
                }
            } else {
                std::vector<float> heapBuf(item.jointCount * 16);
                for (uint32_t k = 0; k < item.jointCount; ++k) {
                    std::memcpy(&heapBuf[k * 16],
                                item.boneMatrices[k].ptr(),
                                sizeof(float) * 16);
                }
                if (material.boneBlockBinding != shader::InvalidBinding) {
                    material.shader.setUniformBlock(material.boneBlockBinding,
                                                    heapBuf.data(), byteCount);
                } else {
                    const shader::BindingId bonesUniform =
                        material.shader.getUniformBinding("bones");
                    if (bonesUniform != shader::InvalidBinding) {
                        material.shader.setUniform(bonesUniform, heapBuf.data(), byteCount);
                    }
                }
            }
        }

        shader::DrawCallContext ctx;
        ctx.viewId = viewId;
        ctx.state  = defaultState;
        material.shader.submit(ctx);
        ++drawCount;
    }

    return drawCount;
}

} // namespace ayt::render::detail
