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
    //      exponential fog blit on view 13 (before Final PP=14).
    //   3. depth source — Deferred samples GBuffer RT2 worldPos via
    //      ctx.gbufferPass->gbufferMotionRt(); Forward / missing
    //      gbuffer ⇒ DepthHazePass returns 0 (safe no-haze).
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
    // knobs. Eight-tap worldPos-sphere occlusion pass (simplified;
    // no normal reconstruction, no GTAO). Visible only on the
    // Deferred pipeline (render() central `ssaoPassEnabled` also
    // gates on `gbufferPass != nullptr` so Forward never sees it).
    //
    // K-SSAO-1 invariant (must survive A2 wire + A3 composite):
    //   ssaoEnabled=false OR ssaoStrength<=0 ⇒ render() central
    //   `ssaoPassEnabled = false` ⇒ FG compile culls SSAOTexture
    //   ⇒ FrameGraph::resolve returns invalid ⇒ SSAOPass::execute
    //   early-returns 0 ⇒ zero draw, zero alloc. PostProcessPass
    //   composite gate (A3) then binds sceneColor on the SSAO
    //   sampler slot (semantic invalid → fallback path) and the
    //   FS branchless `step(0.0001, ssaoStrength.x)` collapses
    //   the contribution to 0 — byte-equivalent composite to
    //   pre-A3 renders.
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

    // V1 GBuffer Debug (2026-07-24) — host knobs for the
    // independent GBuffer channel debug pass (option-B second
    // viewport on view 250, not main-frame replacement). Default
    // OFF so pre-V1 brace-init sites keep byte-identical behavior;
    // hosts opt in via Renderer::setGBufferDebugEnabled/Channel
    // (mirror SSAO setters in AYRenderer.h:299-308).
    //
    // K-GBD-1: enabled=false OR gbufferPass==nullptr OR
    // uninit/Noop ⇒ render() central gate false ⇒ FBO NOT
    // created (zero alloc) ⇒ ctx.gbufferDebugFbo invalid ⇒
    // execute() returns 0 (zero draw). Enforced at host central
    // gate + execute step-4 (double-check).
    //
    // K-GBD-3 (V1 only, will be lifted in V2): gbufferDebugChannel
    // WorldPos(2) / Motion(3) BOTH alias gbufferMotionRt() because
    // GBufferPass has not split a real motion RT from worldPos
    // (RT2 = worldPos RGBA16F per GBufferPass.cpp:49-60). V2 will
    // add a real Motion RT and re-route channel 3 to it.
    bool              gbufferDebugEnabled  = false;
    uint8_t           gbufferDebugChannel  = 0;     // GBufferDebugChannel 0=Albedo..4=Depth
};

} // namespace ayt::render::detail
