#pragma once

#include "aymath/MathTypes.h"

#include <cstdint>

namespace ayt::render::detail
{

// §5.5 cleanup (2026-07-22) — FrameContext no longer holds F1-diagnostic
// shadow fields (shadowFboIdx / lightViewProj / lightIndex). Those were
// the §5.5 PR-F1' C' forbidden combos (FrameContext shadow writeback
// combined with default-on Shadow). E5 ships default-on Shadow WITHOUT
// that writeback path, and the diagnostic code-path is now permanently
// retired. The only shadow-related state remaining in FrameContext is
// `shadowMapId` (an E1-shipped POD tail — semantic-free, kept so we
// have a stable place to bind an optional per-frame shadow index
// without growing the layout again).
struct FrameContext {
    ayt::math::Float4x4 view            = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 projection     = ayt::math::Float4x4::identity();
    ayt::math::FVector3 cameraPosition  = ayt::math::FVector3(0.0f, 0.0f, 4.0f);
    ayt::math::FVector3 lightDirection  = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    ayt::math::FVector3 lightColor      = ayt::math::FVector3(1.0f, 1.0f, 1.0f);

    float             timeSeconds      = 0.0f;
    float             bloomStrength    = 0.0f;
    float             exposure         = 1.0f;
    // Display gamma encode (pow(c, 1/gamma)). 2.2 ≈ sRGB OETF.
    float             gamma            = 2.2f;

    enum class TonemapMode : uint8_t {
        None     = 0,
        Reinhard = 1,
        ACES     = 2,
    };
    TonemapMode       tonemapMode      = TonemapMode::None;

    uint32_t          shadowMapId      = 0;

    // P4.2 (§P4, 2026-07-22) — global shadow bias for receivers.
    // Mirror of Renderer::setShadowBias(float); consumed by the
    // forward / transparent passes when binding the shadow sampler.
    // Units: same as the receiver Phoskia `shadowBias` property
    // (ndc01 space; the receiver fragment does `refNdc01 + bias`
    // before the depth comparison, see AYShadowShaderSources.h:211
    // + the simple_lit_shadow.phoskia receiver contract). Default
    // 0.003f matches the Phoskia property default (see
    // AYShadowShaderSources.h:81). Host callers that want per-material
    // control still call setMaterialVec3(material, "shadowBias", v)
    // — the global value here is a multiplier applied during
    // tryBindShadowSampler() AFTER the per-material uniform write,
    // so it acts as a frame-level offset (set 0 to disable).
    float             shadowBias       = 0.003f;

    // §S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — DepthHaze
    // knobs (exponential `1 - exp(-density * dist)` distance fog).
    //
    // K3 invariants (must survive §S4c PostProcessPass haze sampler
    // wire + §S4d Editor default-knob polish):
    //   1. hazeEnabled == false OR hazeStrength <= 0 ⇒ DepthHazePass
    //      early-returns 0 (no FBO ensure, no fullscreen blit, no
    //      HalfRes allocation); PostProcessPass falls back to bind
    //      sceneColor on the haze slot; FS branchless composite
    //      collapses to `raw * (1 + 0) = raw` — byte-equivalent to
    //      pre-S4 renders.
    //   2. hazeEnabled == true ⇒ DepthHazePass ensureFbo at
    //      (W+1)/2 × (H+1)/2 (mirror BloomExtractPass) + Phoskia
    //      exponential fog blit on view 14 (S4a view-id lock).
    //   3. depth source — currently opaque (Deferred reads GBuffer
    //      RT2 via ctx.gbufferPass->gbufferMotionRt() which today
    //      stores worldPos-encoded motion vectors per §P5 B4c; the
    //      fallback "no motion available" worldPos is the camera
    //      position itself, so the exponential collapse to `raw *
    //      (1 - exp(0)) = raw * 0 = 0` is the S4b safe default
    //      — see DepthHazePass::execute for the per-source
    //      decision tree).
    //
    // Default = haze OFF (hazeEnabled=false, hazeStrength=0) so
    // FrameContext's brace-init default keeps the pre-S4 byte-
    // identical behavior on every existing test site. Hosts enable
    // haze by writing hazeEnabled=true + a non-zero hazeStrength;
    // the Editor §S4d knob wraps this with setDepthHazeEnabled().
    //
    // Why these live on FrameContext (not PassExecContext): the
    // §5.3 rule was "no FrameContext GBuffer slot / no shadow FBO
    // slot / no lastFrameShadowFbo" — those are GPU handles, not
    // POD knobs. P4.2 (shadowBias) shipped the precedent of POD
    // fog knobs (here: exponential model parameter) on FrameContext.
    // DepthHazePass reads them via ctx.frame.haze* once per frame.
    bool              hazeEnabled      = false;
    float             hazeStrength     = 0.0f;
    float             hazeDensity      = 0.02f;  // exp falloff (1/units)
    ayt::math::FVector3 hazeColor      = ayt::math::FVector3(0.55f, 0.65f, 0.78f);
};

} // namespace ayt::render::detail
