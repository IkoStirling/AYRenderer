// RenderPass.cpp — U1++ lifted helper. Color-uniform upload used to
// be duplicated byte-for-byte in ForwardOpaquePass::flushMaterial and
// TransparentPass::execute (now both call this single source of truth).

#include "detail/RenderPass.h"
#include "detail/GpuResources.h"
#include "detail/ShadowPass.h"

#include "AYShaderResource.h"  // ShaderResource::getUniformBinding / hasUniformBinding / setUniform

#include <bgfx/bgfx.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ayt::render::detail
{

void RenderPass::resolveAndApplyColorUniforms(GpuMaterial& material)
{
    // Lazily resolve colorBinding on first use (hot-reload friendly:
    // RenderResourceManager::setMaterialColor pre-populates this too,
    // so on a normal path the cached binding is non-invalid before
    // we get here — this block is the safety net for materials that
    // are dispatched without going through setMaterialColor first).
    if (material.colorBinding == ayt::shader::InvalidBinding) {
        material.colorBinding = material.shader.getUniformBinding("baseColor");
        if (material.colorBinding == ayt::shader::InvalidBinding) {
            material.colorBinding = material.shader.getUniformBinding("color");
        }
    }
    // Re-validate the cached binding each frame in case the shader
    // was hot-reloaded out from under us and the cached BindingId no
    // longer maps to a live uniform slot. Cheaper than re-resolving
    // and keeps the invariant "colorBinding == InvalidBinding  ⇔
    // shader has no baseColor/color uniform".
    if (material.colorBinding != ayt::shader::InvalidBinding) {
        if (!material.shader.hasUniformBinding(material.colorBinding)) {
            material.colorBinding = ayt::shader::InvalidBinding;
        }
    }
    if (material.colorBinding != ayt::shader::InvalidBinding) {
        if (material.hasColorOverride) {
            material.shader.setUniform(material.colorBinding,
                                       material.colorOverride.ptr(),
                                       sizeof(float) * 4);
        } else {
            // Phoskia property defaults are not guaranteed on D3D;
            // use a neutral white tint as the no-override baseline.
            const float defaultBaseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            material.shader.setUniform(material.colorBinding,
                                       defaultBaseColor,
                                       sizeof(defaultBaseColor));
        }
    }
}

// PR-F2 (2026-07-21) — see RenderPass.h. Body lifted out of the
// header so the `ShadowPass*` parameter doesn't need a complete
// type at the inline definition site (only forward-declaration
// from the header). Caller passes non-null to actually exercise
// the upload path; nullptr / invalid FBO are no-ops.
void tryBindShadowSampler(shader::ShaderResource& shader,
                          BGFXAdapter& adapter,
                          const ShadowPass* shadowPass)
{
    if (shadowPass == nullptr || !bgfx::isValid(shadowPass->shadowFbo())) {
        return;
    }
    trySetUniformMat4(shader,
                      "u_lightViewProj", "lightViewProj",
                      shadowPass->lightViewProj());

    const shader::BindingId shadowBinding =
        shader.getTextureBinding("shadowMap");
    if (shadowBinding == shader::InvalidBinding) {
        return;
    }
    const bgfx::TextureHandle shadowTex =
        adapter.getFboAttachment(shadowPass->shadowFbo(), 0);
    if (!BGFXAdapter::isValid(shadowTex)) {
        return;
    }
    shader.setTexture(0, shadowBinding, toShaderTexture(shadowTex));
}

// PR-F3 (2026-07-21) — see RenderPass.h. Body lifted from
// ForwardOpaquePass's execute() inner block; ShadowPass's caster
// loop now calls the same helper so both sites upload identical
// bytes and the threshold / fallback behavior cannot drift.
//
// `castSkinnedValue` is the uniform toggle the depth caster reads
// via Phoskia `property castSkinned`. Passing 0 from FO paths is
// allowed (FO programs don't have that uniform — the helper early-
// outs when `castSkinnedBinding == InvalidBinding`).
//
// Stack-path for ≤ 16 joints (covers all current AYEngine skinned
// assets; Skeleton UBO can hold up to 128 but the per-frame upload
// cost is dominated by memcpy, not allocation). Heap-path for
// larger counts preserves the pre-F3 behavior: std::vector<float>
// scratch space, header-only allocator.
void tryUploadBonePalette(shader::ShaderResource& shader,
                          shader::BindingId skeletonBinding,
                          shader::BindingId castSkinnedBinding,
                          uint8_t castSkinnedValue,
                          const DrawItem& item)
{
    const bool hasBones = item.boneMatrices != nullptr && item.jointCount > 0;

    // Set the castSkinned uniform ONLY when (a) it's a binding on
    // this program and (b) we have bones. Pre-F3 SkinnedLit
    // materials skip this branch (their castSkinned binding is
    // Invalid — there's no such property in their .phoskia).
    if (hasBones
        && castSkinnedBinding != shader::InvalidBinding) {
        shader.setUniform(castSkinnedBinding,
                          &castSkinnedValue, sizeof(castSkinnedValue));
    }

    if (!hasBones) {
        return;
    }

    const size_t byteCount = static_cast<size_t>(item.jointCount) * 64;
    if (byteCount <= 1024) {
        float stackBuf[1024 / sizeof(float)];
        for (uint32_t k = 0; k < item.jointCount; ++k) {
            std::memcpy(&stackBuf[k * 16],
                        item.boneMatrices[k].ptr(),
                        sizeof(float) * 16);
        }
        if (skeletonBinding != shader::InvalidBinding) {
            shader.setUniformBlock(skeletonBinding, stackBuf, byteCount);
        } else {
            const shader::BindingId bonesUniform =
                shader.getUniformBinding("bones");
            if (bonesUniform != shader::InvalidBinding) {
                shader.setUniform(bonesUniform, stackBuf, byteCount);
            } else {
                static uint32_t s_missingBoneBindingLog = 0;
                if (s_missingBoneBindingLog < 3) {
                    std::fprintf(stderr,
                                 "[RenderPass] skinned draw skipped bone upload "
                                 "(Skeleton UBO / bones[] binding missing)\n");
                    ++s_missingBoneBindingLog;
                }
            }
        }
    } else {
        std::vector<float> heapBuf(static_cast<size_t>(item.jointCount) * 16);
        for (uint32_t k = 0; k < item.jointCount; ++k) {
            std::memcpy(&heapBuf[k * 16],
                        item.boneMatrices[k].ptr(),
                        sizeof(float) * 16);
        }
        if (skeletonBinding != shader::InvalidBinding) {
            shader.setUniformBlock(skeletonBinding, heapBuf.data(), byteCount);
        } else {
            const shader::BindingId bonesUniform =
                shader.getUniformBinding("bones");
            if (bonesUniform != shader::InvalidBinding) {
                shader.setUniform(bonesUniform, heapBuf.data(), byteCount);
            }
        }
    }
}

} // namespace ayt::render::detail
