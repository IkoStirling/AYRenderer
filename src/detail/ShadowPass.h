#pragma once

#include "AYRenderScene.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/ShadowCaster.h"
#include "detail/ShadowMapResources.h"

#include "AYShadowConfig.h"

#include "aymath/MathTypes.h"

#include <bgfx/bgfx.h>

#include <climits>
#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class ShadowPass : public RenderPass {
public:
    static constexpr uint16_t kDefaultShadowMapSize = 2048;
    static constexpr float    kDefaultFrustumRadius = ayt::render::kShadowDefaultFrustumRadius;
    static constexpr float    kShadowNearPlane      = ayt::render::kShadowNearPlane;
    static constexpr float    kShadowFarPlane       = ayt::render::kShadowFarPlane;
    // Composite view map: 1 = caster FBO, 2 = resolve blit (must differ).
    static constexpr uint8_t  kShadowViewId        = 1;
    static constexpr uint8_t  kShadowResolveViewId = 2;

    ShadowPass() = default;
    ~ShadowPass() override;

    std::string_view name() const override { return "Shadow"; }

    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept
    {
        return _mapResources.isValid() && hasSampleableShadow();
    }

    bool hasSampleableShadow() const noexcept
    {
        return _mapResources.hasSampleableShadow();
    }

    bool lastBlitOk() const noexcept { return _mapResources.lastBlitOk(); }

    void setShadowMapSize(uint16_t size) noexcept { _requestedSize = size; }
    uint16_t shadowMapSize() const noexcept { return _requestedSize; }

    void setPcfEnabled(bool enabled) noexcept { _pcfEnabled = enabled; }
    bool pcfEnabled() const noexcept { return _pcfEnabled; }

    const ayt::math::Float4x4& lightView() const noexcept { return _lightView; }
    const ayt::math::Float4x4& lightProj() const noexcept { return _lightProj; }
    const ayt::math::Float4x4& lightViewProj() const noexcept { return _lightViewProj; }
    const float* lightViewProjColumnMajor() const noexcept { return _lightViewProjCol; }

    bgfx::FrameBufferHandle shadowFbo() const noexcept { return _mapResources.frameBuffer(); }
    bgfx::TextureHandle shadowSampleTexture() const noexcept
    {
        // Sample the caster color RT (post view-1). Do not require blit→resolve:
        // empty resolve sampled as 0 → pure-black Game View.
        return _mapResources.sampleTexture();
    }

    void destroyResources(BGFXAdapter& adapter);

private:
    ShadowMapResources         _mapResources;
    ShadowCaster               _shadowCaster;
    uint16_t                   _requestedSize = kDefaultShadowMapSize;
    bool                       _pcfEnabled    = true;

    ayt::math::Float4x4        _lightView        = ayt::math::Float4x4::identity();
    ayt::math::Float4x4        _lightProj        = ayt::math::Float4x4::identity();
    ayt::math::Float4x4        _lightViewProj    = ayt::math::Float4x4::identity();
    float                      _lightViewCol[16] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    float                      _lightProjCol[16] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    float                      _lightViewProjCol[16] = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };

    uint32_t                   _frameCounter        = 0;
};

} // namespace ayt::render::detail
