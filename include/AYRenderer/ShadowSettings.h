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

    // §P4 L5 (2026-08-24) — bumped v14 → v15 to invalidate
    // any FBO + resolve texture already cached by
    // ShadowMapResources::ensure (the build stamp is the
    // invalidation key for `_buildStamp != buildStamp`). The
    // audit patches ShadowMapResources with new
    // rate-limited diagnostics + L4's `kProbeLogLimit` for the
    // resolve-fail log; downstream consumers shouldn't see
    // stale FBO handles across the bump.
    static constexpr const char* kPipelineBuildStamp = "v15-shadow-diag-bump";
    // §P4 L6 (2026-08-24) — bumped v6 → v7 to invalidate the
    // shader cache for ShadowCaster after the audit patches:
    // M2 env::get consolidation, M3 rate-limited acquire-fail
    // diagnostic, M11 colorOverride ternary removal, M12
    // slot-name helper hoisting.
    static constexpr const char* kCasterCacheKey     = "shadow_caster_phoskia_v7_audit_p4";
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
