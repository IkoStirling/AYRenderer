// RenderPass.cpp — U1++ lifted helper. Color-uniform upload used to
// be duplicated byte-for-byte in ForwardOpaquePass::flushMaterial and
// TransparentPass::execute (now both call this single source of truth).

#include "detail/RenderPass.h"
#include "detail/BgfxMatrix.h"
#include "detail/GpuResources.h"
#include "detail/ShadowDebug.h"
#include "detail/ShadowPass.h"
#include "AYRenderer/ShadowConfig.h"
#include "AYRenderer/ShadowDiagnostics.h"

#include "AYShader/ShaderResource.h"

#include <bgfx/bgfx.h>
#include <AYIO/Env.h>
#include <cstdio>
#include <string>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace ayt::render::detail
{

namespace {

// §P2 M14 / L2 (2026-08-24) — shared env-var bool parser. Used by
// tryBindShadowSampler (force-lit / use-map) and RenderAssetBridge
// (Phoskia toggle). Truth table:
//   unset / empty  → fallback
//   "1","true","yes","on"  → true
//   "0","false","no","off" → false
// Anything else → fallback (with one stderr log per unique garbage value).
bool parseEnvBool(const char* envName, bool fallback) noexcept
{
    const std::string raw = ayt::io::env::get(envName).value_or("");
    if (raw.empty()) {
        return fallback;
    }
    std::string v;
    v.reserve(raw.size());
    for (char c : raw) {
        if (c >= 'A' && c <= 'Z') v.push_back(c - 'A' + 'a');
        else v.push_back(c);
    }
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        return false;
    }
    // Garbage: log once per unique value (avoid spam from same-bad
    // value hitting every frame) and return fallback.
    static std::atomic<uint32_t> s_logged{0};
    if (s_logged.load() < 4u) {
        std::fprintf(stderr,
                     "[RenderPass] parseEnvBool(%s): unrecognized value '%s', "
                     "falling back to %s\n",
                     envName, raw.c_str(),
                     fallback ? "true" : "false");
        s_logged.fetch_add(1u);
    }
    return fallback;
}

} // namespace

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

// Phase 5 — receiver bind. Contract: AYRenderer/ShadowReceiverContract.h
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
    //
    // §P2 M14 (2026-08-24) — routed through shared parseEnvBool helper
    // (matches the AY_SHADOW_USE_SC / AY_SHADOW_USE_PHOSKIA handling in
    // RenderAssetBridge; truth table: 1/true/yes/on vs 0/false/no/off).
    const bool forceLit = []() noexcept {
        const bool forcedLit =
            parseEnvBool("AY_SHADOW_FORCE_LIT", false);
        if (forcedLit) return true;
        // AY_SHADOW_USE_MAP=0 → force lit; default true (sample map).
        const bool useMap = parseEnvBool("AY_SHADOW_USE_MAP", true);
        return !useMap;
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
        // §P2 M8 (2026-08-24) — was silent; receivers with shadowMap sampler
        // would sample unbound texture (undefined on real GPUs). Log at L2_Frame
        // (which is the project's "first N frames" diagnostic level) AND at L4
        // verbose, plus emit an explicit "fallback unavailable" diagnostic so a
        // Noop backend + shader declaring shadowMap surfaces immediately.
        if (ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L2_Frame)) {
            static uint32_t s_errLog = 0;
            if (s_errLog < 4) {
                std::fprintf(stderr,
                             "[ShadowDbg][ERROR] shadow bind FAILED — "
                             "neither producer FBO nor lit-fallback texture "
                             "valid (pass=%p receive=%d sampleReady=%d "
                             "adapterInit=%d). Receiver shader will read "
                             "unbound texture.\n",
                             static_cast<const void*>(shadowPass),
                             wantSample ? 1 : 0,
                             shadowPass != nullptr && shadowPass->hasSampleableShadow() ? 1 : 0,
                             adapter.isInitialized() ? 1 : 0);
                ++s_errLog;
            }
        }
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
// `castSkinnedValue` is the uniform toggle read by every skinned-capable
// pass. Programs without that property keep an InvalidBinding and simply
// skip the toggle write.
//
// Stack-path for <= 16 draw-local joints; larger valid palettes use a heap
// scratch buffer. The 128 value is a backend per-draw capability, never a
// limit on the complete skeleton stored in item.boneMatrices.
void tryUploadBonePalette(shader::ShaderResource& shader,
                          shader::BindingId skeletonBinding,
                          shader::BindingId castSkinnedBinding,
                          uint8_t castSkinnedValue,
                          const DrawItem& item)
{
    bool hasBones = item.boneMatrices != nullptr && item.jointCount > 0;
    if (hasBones && item.jointCount > kUniformSkinPaletteCapacity) {
        static uint32_t s_paletteCapacityLog = 0;
        if (s_paletteCapacityLog < 3u) {
            std::fprintf(stderr,
                         "[RenderPass] skin palette %u exceeds backend capacity %u; "
                         "draw uses bind pose\n",
                         item.jointCount, kUniformSkinPaletteCapacity);
            ++s_paletteCapacityLog;
        }
        hasBones = false;
    }
    if (hasBones && item.boneRemap != nullptr) {
        if (item.skeletonJointCount == 0u) {
            hasBones = false;
        } else {
            for (uint32_t k = 0; k < item.jointCount; ++k) {
                if (item.boneRemap[k] >= item.skeletonJointCount) {
                    static uint32_t s_invalidRemapLog = 0;
                    if (s_invalidRemapLog < 3u) {
                        std::fprintf(stderr,
                                     "[RenderPass] skin palette slot %u maps to joint %u/%u; "
                                     "draw uses bind pose\n",
                                     k, item.boneRemap[k], item.skeletonJointCount);
                        ++s_invalidRemapLog;
                    }
                    hasBones = false;
                    break;
                }
            }
        }
    }

    // Always write the toggle when the program exposes it. Uniform values
    // persist across bgfx draws, so leaving the previous skinned draw's value
    // at 1 would make a following static caster index an absent bone palette.
    if (castSkinnedBinding != shader::InvalidBinding) {
        const float castSkinned[4] = {
            hasBones && castSkinnedValue != 0 ? 1.0f : 0.0f,
            0.0f, 0.0f, 0.0f
        };
        shader.setUniform(castSkinnedBinding,
                          castSkinned, sizeof(castSkinned));
    }

    if (!hasBones) {
        return;
    }

    const size_t byteCount = static_cast<size_t>(item.jointCount) * 64;
    if (byteCount <= 1024) {
        float stackBuf[1024 / sizeof(float)];
        for (uint32_t k = 0; k < item.jointCount; ++k) {
            const uint32_t joint = item.boneRemap != nullptr ? item.boneRemap[k] : k;
            toBgfxColumnMajor(item.boneMatrices[joint], &stackBuf[k * 16]);
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
            const uint32_t joint = item.boneRemap != nullptr ? item.boneRemap[k] : k;
            toBgfxColumnMajor(item.boneMatrices[joint], &heapBuf[k * 16]);
        }
        if (skeletonBinding != shader::InvalidBinding) {
            shader.setUniformBlock(skeletonBinding, heapBuf.data(), byteCount);
        } else {
            const shader::BindingId bonesUniform =
                shader.getUniformBinding("bones");
            if (bonesUniform != shader::InvalidBinding) {
                shader.setUniform(bonesUniform, heapBuf.data(), byteCount);
            } else {
                // §P2 M6 (2026-08-24) — heap path was silent. Same
                // rate-limited diagnostic as the stack path; the
                // condition is rare (jointCount > 16) but the silent
                // version made diagnostic hunting painful.
                static uint32_t s_missingBoneBindingLog = 0;
                if (s_missingBoneBindingLog < 3) {
                    std::fprintf(stderr,
                                 "[RenderPass] skinned draw (heap) skipped bone "
                                 "upload (Skeleton UBO / bones[] binding missing)\n");
                    ++s_missingBoneBindingLog;
                }
            }
        }
    }
}

} // namespace ayt::render::detail
