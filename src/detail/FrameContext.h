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
    // initialize. Drives future post-process shader uniforms (R5+
    // plans: bloom intensity pulse, time-of-day color grading, etc.).
    // Today PostProcessPass::execute() is a no-op and does not consume
    // it; once R5+ lands it will read this field into a float uniform
    // named "u_time" (the canonical Phoskia timing uniform name across
    // the codebase). Renderer::render() sets this from a wall-clock
    // double-cast-to-float on the main thread — main thread only,
    // matching the rest of FrameContext.
    //
    // Default 0.0f preserves source compatibility for callers that
    // construct FrameContext explicitly (existing tests). Adding the
    // field does NOT change any existing layout — it's appended after
    // the 5 legacy fields with default-init semantics.
    float             timeSeconds      = 0.0f;
};

} // namespace ayt::render::detail
