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
    // Screen-space ripple (UV warp). 0 = off (identity sampling).
    float             rippleStrength   = 0.0f;
    float             rippleFrequency  = 28.0f;
    float             rippleSpeed      = 4.0f;

    enum class TonemapMode : uint8_t {
        None     = 0,
        Reinhard = 1,
        ACES     = 2,
    };
    TonemapMode       tonemapMode      = TonemapMode::None;

    uint32_t          shadowMapId      = 0;
};

} // namespace ayt::render::detail
