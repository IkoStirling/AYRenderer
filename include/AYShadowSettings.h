#pragma once

namespace ayt::render
{

// Tunables and build identity for the shadow subsystem.
// Matrix near/far must stay consistent with ShadowMatrixBuilder (Phase 3).
struct ShadowSettings {
    static constexpr float kNearPlane            = 0.1f;
    static constexpr float kDefaultFrustumRadius = 12.0f;
    static constexpr float kFarPlane             = kDefaultFrustumRadius * 2.0f;
    static constexpr float kDepthRange           = kFarPlane - kNearPlane;

    // bgfx example 16 default shadow bias magnitude.
    static constexpr float kBiasDefault = 0.003f;

    static constexpr float kLitMin   = 0.20f;
    static constexpr float kLitScale = 0.65f;

    static constexpr const char* kPipelineBuildStamp = "v13-phase7-vec4-abi";
    static constexpr const char* kCasterCacheKey     = "shadow_caster_phoskia_v4_no_init";
};

// Legacy names — keep until all call sites migrate (Phase 6 cleanup).
inline constexpr float kShadowNearPlane            = ShadowSettings::kNearPlane;
inline constexpr float kShadowDefaultFrustumRadius = ShadowSettings::kDefaultFrustumRadius;
inline constexpr float kShadowFarPlane             = ShadowSettings::kFarPlane;
inline constexpr float kShadowDepthRange           = ShadowSettings::kDepthRange;
inline constexpr float kShadowBiasDefault          = ShadowSettings::kBiasDefault;
inline constexpr float kShadowLitMin               = ShadowSettings::kLitMin;
inline constexpr float kShadowLitScale             = ShadowSettings::kLitScale;

inline constexpr const char* kShadowPipelineBuildStamp = ShadowSettings::kPipelineBuildStamp;
inline constexpr const char* kShadowCasterCacheKey     = ShadowSettings::kCasterCacheKey;

} // namespace ayt::render
