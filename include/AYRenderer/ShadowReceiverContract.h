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
// Texture stages are shader-reflection owned. PBR currently declares base
// color, opacity, then shadow; passes must query stages rather than assume 0/1.
struct ShadowReceiverContract {
    static constexpr std::string_view kShadowMapName     = "shadowMap";
    static constexpr std::string_view kLightViewProjName = "u_lightViewProj";
    static constexpr std::string_view kLightViewProjAlt  = "lightViewProj";
    static constexpr std::string_view kShadowBiasName    = "shadowBias";
    static constexpr std::string_view kShadowDebugName   = "shadowDebugVis";
    static constexpr std::string_view kShadowMapTexelName = "shadowMapTexel";
    static constexpr std::string_view kShadowPcfName     = "shadowPcf";
    static constexpr uint8_t          kShadowSamplerStage = 1;

    // §P4 L12 (2026-08-24) — kClearedOccluderMin is the
    // sentinel for "no occluder in this pixel ⇒ fully lit".
    // ShadowMapResources clears the shadow color RT to
    // 0xffffffff (RGBA8 = 1.0 in every channel) before each
    // caster pass; depth values stored in .r are therefore
    // exactly 1.0 in any pixel the caster never touched.
    // The shader compares `occluder.r >= kClearedOccluderMin`
    // to short-circuit "lit" without sampling depth. 0.999
    // (vs the more obvious 0.9999 or 1.0) leaves headroom
    // for half-precision FS round-off when shadow casts are
    // RGBA8-encoded depths. Tightening this to >= 1.0 would
    // cause self-shadow acne on the first frame after a
    // shadowPass clear (RGBA8 round-off makes the encoded
    // "1.0" actually 0.99853...).
    static constexpr float kClearedOccluderMin = 0.999f;
    static constexpr float kDefaultBias = ShadowSettings::kBiasDefault;

    // True when the draw item should sample the real shadow map.
    static bool shouldSampleShadowMap(ShadowFlags flags) noexcept
    {
        return receivesShadow(flags);
    }
};

} // namespace ayt::render
