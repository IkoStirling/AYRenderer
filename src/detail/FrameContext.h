#pragma once

#include "AYMath/MathTypes.h"

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
    // before the depth comparison, see AYRenderer/ShadowShaderSources.h:211
    // + the simple_lit_shadow.phoskia receiver contract). Default
    // 0.003f matches the Phoskia property default (see
    // AYRenderer/ShadowShaderSources.h:81). Host callers that want per-material
    // control still call setMaterialVec3(material, "shadowBias", v)
    // — the global value here is a multiplier applied during
    // tryBindShadowSampler() AFTER the per-material uniform write,
    // so it acts as a frame-level offset (set 0 to disable).
    float             shadowBias       = 0.003f;

    // §S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — DepthHaze
    // knobs (exponential `1 - exp(-density * dist)` distance fog).
    //
    // Current FrameGraph invariants:
    //   1. hazeEnabled == false OR hazeStrength <= 0 ⇒ DepthHazePass
    //      early-returns 0 and FrameGraph does not declare HazeColor;
    //      downstream source routing stays on the underlying scene.
    //   2. hazeEnabled == true ⇒ DepthHazePass writes full-resolution
    //      RGBA16F HazeColor on view 13, before PostProcess view 15.
    //   3. Deferred samples GBuffer RT2 world position and RT3 geometry
    //      coverage. Forward / missing GBuffer returns 0 (safe no-haze).
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

    // §A1 SSAO MVP (2026-07-24, mid-term FG MVP SSAO Gate) — SSAO
    // knobs. Eight-tap TBN-oriented occlusion pass using GBuffer world
    // position, normal, RT3 coverage, and view-space depth. Visible only on the
    // Deferred pipeline (render() central `ssaoPassEnabled` also
    // gates on `gbufferPass != nullptr` so Forward never sees it).
    //
    // Fail-closed invariant:
    //   disabled/zero/non-finite settings or an incomplete GBuffer -> SSAO ->
    //   Lighting chain ⇒ render() central
    //   `ssaoPassEnabled = false` ⇒ FG compile culls SSAOTexture
    //   ⇒ FrameGraph::resolve returns invalid ⇒ SSAOPass::execute
    //   early-returns 0 ⇒ zero draw, zero alloc. Lighting uploads zero AO
    //   strength, while DepthHaze/PostProcess never sample SSAO directly.
    //
    // Default = ALL OFF (enabled=false / strength=0) so
    // FrameContext brace-init keeps the pre-SSAO byte-identical
    // behavior on every existing test site. Hosts enable SSAO by
    // writing ssaoEnabled=true + a non-zero ssaoStrength +
    // ensuring a Deferred pipeline is mounted. Editor §S2 v1
    // wraps this with Renderer::setSsaoEnabled / setSsaoStrength.
    //
    // Why these live on FrameContext (not PassExecContext): POD
    // knobs analogous to the P4.2 shadowBias / §S4b hazeEnabled
    // precedent. SSAOPass reads them via ctx.frame.ssao* once per
    // frame. ABI-lock the trailing-default so pre-A1 / A2 /
    // A3 tests stay compiling without edits via C++14 trailing-
    // default behavior.
    bool              ssaoEnabled      = false;
    float             ssaoStrength     = 0.0f;
    float             ssaoRadius       = 0.5f;   // world-units; sample-sphere radius
    float             ssaoBias         = 0.025f; // depth-compare epsilon (world-units)

    // Deferred GBuffer fullscreen overlay. Default OFF gates before pass GPU
    // resource creation. Channels: albedo, encoded normal, world position,
    // packed material scalars, depth, material model.
    bool              gbufferDebugEnabled  = false;
    uint8_t           gbufferDebugChannel  = 0; // GBufferDebugChannel
};

} // namespace ayt::render::detail
