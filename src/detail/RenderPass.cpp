// RenderPass.cpp — U1++ lifted helper. Color-uniform upload used to
// be duplicated byte-for-byte in ForwardOpaquePass::flushMaterial and
// TransparentPass::execute (now both call this single source of truth).

#include "detail/RenderPass.h"
#include "detail/BgfxMatrix.h"
#include "detail/GpuResources.h"
#include "detail/ShadowDebug.h"
#include "detail/ShadowPass.h"
#include "AYShadowConfig.h"
#include "AYShadowDiagnostics.h"

#include "AYShaderResource.h"

#include <bgfx/bgfx.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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

// Phase 5 — receiver bind. Contract: AYShadowReceiverContract.h
void tryBindShadowSampler(shader::ShaderResource& shader,
                          BGFXAdapter& adapter,
                          const ShadowPass* shadowPass,
                          ShadowFlags flags,
                          float bias)
{
    using Contract = ayt::render::ShadowReceiverContract;

    const shader::BindingId shadowBinding =
        shader.getTextureBinding(Contract::kShadowMapName.data());
    if (shadowBinding == shader::InvalidBinding) {
        return;
    }

    // Isolation: AY_SHADOW_FORCE_LIT=1 → white fallback (no map).
    // Default: sample the map (AY_SHADOW_USE_MAP=0 forces lit).
    const bool forceLit = []() noexcept {
        if (const char* v = std::getenv("AY_SHADOW_FORCE_LIT")) {
            return v[0] != '\0' && v[0] != '0';
        }
        if (const char* v = std::getenv("AY_SHADOW_USE_MAP")) {
            return !(v[0] != '\0' && v[0] != '0');
        }
        return false;
    }();

    const bool wantSample = !forceLit && Contract::shouldSampleShadowMap(flags);
    bgfx::TextureHandle shadowTex = BGFX_INVALID_HANDLE;
    bool usedFallback = false;
    bool lvpUploaded = false;

    if (wantSample
        && shadowPass != nullptr
        && shadowPass->hasSampleableShadow()) {
        shadowTex = shadowPass->shadowSampleTexture();

        const shader::BindingId lvpBinding =
            shader.getUniformBinding(Contract::kLightViewProjName.data());
        const shader::BindingId lvpAlt =
            (lvpBinding == shader::InvalidBinding)
                ? shader.getUniformBinding(Contract::kLightViewProjAlt.data())
                : lvpBinding;
        if (lvpAlt != shader::InvalidBinding) {
            shader.setUniform(lvpAlt,
                              shadowPass->lightViewProjColumnMajor(),
                              sizeof(float) * 16);
            lvpUploaded = true;
        }

        const shader::BindingId biasBinding =
            shader.getUniformBinding(Contract::kShadowBiasName.data());
        if (biasBinding != shader::InvalidBinding) {
            // P4.2 (2026-07-22) — bias comes from the caller
            // (FrameContext::shadowBias via Renderer::setShadowBias)
            // instead of the hard-coded Contract::kDefaultBias.
            // Default 0.003f matches the Phoskia property default
            // so existing receivers render identically when the
            // host has not called setShadowBias().
            const float biasPad[4] = {bias, 0.0f, 0.0f, 0.0f};
            shader.setUniform(biasBinding, biasPad, sizeof(biasPad));
        }

        const shader::BindingId texelBinding =
            shader.getUniformBinding(Contract::kShadowMapTexelName.data());
        if (texelBinding != shader::InvalidBinding) {
            const float mapSize = static_cast<float>(
                shadowPass->shadowMapSize() > 0 ? shadowPass->shadowMapSize() : 1u);
            const float texel = 1.0f / mapSize;
            const float texelPad[4] = {texel, texel, mapSize, 0.0f};
            shader.setUniform(texelBinding, texelPad, sizeof(texelPad));
        }

        const shader::BindingId pcfBinding =
            shader.getUniformBinding(Contract::kShadowPcfName.data());
        if (pcfBinding != shader::InvalidBinding) {
            const float pcfOn = shadowPass->pcfEnabled() ? 1.0f : 0.0f;
            const float pcfPad[4] = {pcfOn, 0.0f, 0.0f, 0.0f};
            shader.setUniform(pcfBinding, pcfPad, sizeof(pcfPad));
        }
    }

    if (!BGFXAdapter::isValid(shadowTex)) {
        shadowTex = adapter.getLitShadowFallbackTexture();
        usedFallback = true;
        trySetUniformMat4(shader,
                          Contract::kLightViewProjName.data(),
                          Contract::kLightViewProjAlt.data(),
                          ayt::math::Float4x4::identity());
    }

    if (!BGFXAdapter::isValid(shadowTex)) {
        if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L4_Verbose)) {
            static uint32_t s_failLog = 0;
            if (s_failLog < 2) {
                std::fprintf(stderr,
                             "[ShadowDbg] bind FAILED: no shadow tex "
                             "(pass=%p receive=%d sampleReady=%d)\n",
                             static_cast<const void*>(shadowPass),
                             wantSample ? 1 : 0,
                             shadowPass != nullptr && shadowPass->hasSampleableShadow() ? 1 : 0);
                ++s_failLog;
            }
        }
        return;
    }

    {
        static uint32_t s_shadowBindLog = 0;
        if (s_shadowBindLog < 4) {
            std::fprintf(stderr,
                         "[ShadowBind] bind=%s tex.idx=%u forceLit=%d receive=%d "
                         "sampleReady=%d lvp=%d flags=0x%02x\n",
                         usedFallback ? "fallback" : "map",
                         static_cast<unsigned>(shadowTex.idx),
                         forceLit ? 1 : 0,
                         wantSample ? 1 : 0,
                         shadowPass != nullptr && shadowPass->hasSampleableShadow() ? 1 : 0,
                         lvpUploaded ? 1 : 0,
                         static_cast<unsigned>(flags));
            ++s_shadowBindLog;
        }
    }

    shader.setTexture(Contract::kShadowSamplerStage,
                      shadowBinding,
                      toShaderTexture(shadowTex));

    const shader::BindingId debugBinding =
        shader.getUniformBinding(Contract::kShadowDebugName.data());
    if (debugBinding != shader::InvalidBinding) {
        // Never force debug vis on during force-lit isolation.
        const float debugVis =
            (!forceLit && ayt::render::ShadowDiagnostics::debugVisEnabled()) ? 1.0f : 0.0f;
        const float debugPad[4] = {debugVis, 0.0f, 0.0f, 0.0f};
        shader.setUniform(debugBinding, debugPad, sizeof(debugPad));
    }
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
            toBgfxColumnMajor(item.boneMatrices[k], &stackBuf[k * 16]);
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
            toBgfxColumnMajor(item.boneMatrices[k], &heapBuf[k * 16]);
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
