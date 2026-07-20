#pragma once

#include "aymath/MathTypes.h"

namespace ayt::render::detail
{

struct FrameContext {
    ayt::math::Float4x4 view            = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 projection     = ayt::math::Float4x4::identity();
    ayt::math::FVector3 cameraPosition  = ayt::math::FVector3(0.0f, 0.0f, 4.0f);
    ayt::math::FVector3 lightDirection  = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    ayt::math::FVector3 lightColor      = ayt::math::FVector3(1.0f, 1.0f, 1.0f);

    // P0 (2026-07-20) — monotonic wall-clock seconds since Renderer
    // initialize. Drives post-process shader uniforms (R5+ plans:
    // bloom intensity pulse, time-of-day color grading, etc.).
    // PostProcessPass::execute() reads this into a float uniform named
    // "u_time" (the canonical Phoskia timing uniform name across the
    // codebase). Renderer::render() sets this from a wall-clock
    // double-cast-to-float on the main thread — main thread only,
    // matching the rest of FrameContext.
    //
    // Default 0.0f preserves source compatibility for callers that
    // construct FrameContext explicitly (existing tests). Adding the
    // field does NOT change any existing layout — it's appended after
    // the 5 legacy fields with default-init semantics.
    float             timeSeconds      = 0.0f;

    // R5+ (Phase PostProcess, 2026-07-20) — post-process knobs. All
    // optional; default values give "no effect" (no bloom, neutral
    // exposure, identity tonemap) so existing hosts that never call
    // Renderer::setPostProcess* see the same image as the legacy
    // forward-only pipeline.
    //
    // `bloomStrength` ∈ [0, 1] — how much of the bloomed scene color
    // is added to the un-bloomed scene color. 0 disables; 1 = pure
    // bloom (effectively turns the scene white in the bright zones).
    // Fragment shader uniform: `u_bloomStrength`.
    float             bloomStrength    = 0.0f;

    // `exposure` ∈ [0, ~16] — EV stops applied BEFORE tonemapping.
    // 1.0 = neutral; >1 = brighter, <1 = darker. Fragment uniform:
    // `u_exposure`.
    float             exposure         = 1.0f;

    // Tonemap mode — controls the fragment's final color curve. Names
    // match the canonical Phoskia post-process shader enum. Mode
    // serialized as int (Fragment reads `u_tonemapMode`).
    enum class TonemapMode : uint8_t {
        None     = 0,  // identity (gamma clamp only)
        Reinhard = 1,  // Reinhard operator x/(1+x)
        ACES     = 2,  // ACES filmic approximation
    };
    TonemapMode       tonemapMode      = TonemapMode::None;
};

} // namespace ayt::render::detail
