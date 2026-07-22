#pragma once

#include "AYF1DiagFlags.h"
#include "aymath/MathTypes.h"

#include <cstdint>

namespace ayt::render::detail
{

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

#if AY_F1_DIAG_FRAME_SHADOW
    // F1 diag — DO NOT store bgfx::FrameBufferHandle here.
    // Including <bgfx/bgfx.h> from FrameContext.h pulls bgfx into TUs that
    // also see ayt::memory::MemorySystem and breaks on bgfx::Memory /
    // MemorySystem::instance name collisions (seen in AYRendererSubSystem.cpp).
    // Store the raw handle index only (bgfx invalid = kInvalidHandle).
    static constexpr uint16_t kInvalidShadowFbo = UINT16_MAX;
    uint16_t                shadowFboIdx  = kInvalidShadowFbo;
    ayt::math::Float4x4     lightViewProj = ayt::math::Float4x4::identity();
    uint32_t                lightIndex    = 0;
#endif
};

} // namespace ayt::render::detail
