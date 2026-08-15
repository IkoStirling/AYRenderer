#pragma once

#include "AYRenderer/ShadowSettings.h"
#include "AYRenderer/RenderTypes.h"

#include <string_view>

namespace ayt::render
{

// Phase 5 — receiver material contract for shadow-aware Phoskia shaders.
//
// A material is a shadow *receiver* when it declares ALL of:
//   texture2d shadowMap
//   uniform mat4  u_lightViewProj   (alias: lightViewProj)
//   property      shadowBias        (optional; default ShadowSettings::kBiasDefault)
//   property      shadowDebugVis    (optional; driven by AY_SHADOW_DEBUG)
//
// Depth semantics MUST match ShadowDepthCodec / caster:
//   refNdc01 = (clip.z / clip.w) * 0.5 + 0.5
//   occluder = sample(shadowMap, uv).r
//   lit when occluder >= 0.999 (cleared map) OR refNdc01 < occluder + bias
//
// Bind contract (tryBindShadowSampler):
//   1. No shadowMap sampler on the program → no-op (pre-shadow shaders OK).
//   2. DrawItem without Receive flag → bind lit fallback (≈1.0), full lit.
//   3. ShadowPass sample-ready → bind resolve texture + upload LVP bytes.
//   4. Otherwise → lit fallback + identity LVP (safe fully-lit path).
//
// Texture stage: albedoMap = 0, shadowMap = 1 (FO skips shadowMap in albedo loop).
struct ShadowReceiverContract {
    static constexpr std::string_view kShadowMapName     = "shadowMap";
    static constexpr std::string_view kLightViewProjName = "u_lightViewProj";
    static constexpr std::string_view kLightViewProjAlt  = "lightViewProj";
    static constexpr std::string_view kShadowBiasName    = "shadowBias";
    static constexpr std::string_view kShadowDebugName   = "shadowDebugVis";
    static constexpr std::string_view kShadowMapTexelName = "shadowMapTexel";
    static constexpr std::string_view kShadowPcfName     = "shadowPcf";
    static constexpr uint8_t          kShadowSamplerStage = 1;

    static constexpr float kClearedOccluderMin = 0.999f;
    static constexpr float kDefaultBias = ShadowSettings::kBiasDefault;

    // True when the draw item should sample the real shadow map.
    static bool shouldSampleShadowMap(ShadowFlags flags) noexcept
    {
        return receivesShadow(flags);
    }
};

} // namespace ayt::render
