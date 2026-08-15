#pragma once

#include "AYRenderScene.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/ShadowAtlas.h"
#include "detail/ShadowCaster.h"
#include "detail/ShadowMapResources.h"

#include "AYShadowConfig.h"

#include "AYMath/MathTypes.h"

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
    // §P5.5 C (2026-07-23) — atlas size when per-light shadow is
    // active (4096×4096 default ⇒ 8 sub-rects of 2048×2048 each in
    // a 4×2 grid). Falls back to kDefaultShadowMapSize (single 2048)
    // when no SceneLights instance is wired (pre-C byte-equivalent).
    static constexpr uint16_t kDefaultAtlasSize = kShadowAtlasDefaultSize;

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

    // §P5.5 C (2026-07-23) — per-light shadow wiring. The renderer
    // wires `ctx.perLightShadows` (== `ctx.sceneLights` in current
    // cuts) into the producer before each execute(). When the
    // pointer is non-null, the pass picks castShadow=true slots and
    // produces one atlas sub-rect per caster. When null (default),
    // execute() behaves exactly as pre-C: single directional caster
    // into one atlas-sized sub-rect, count==0 reported.
    void setSceneLightsRef(const ayt::render::SceneLights* lights) noexcept
    {
        _sceneLightsRef = lights;
    }
    const ayt::render::SceneLights* sceneLightsRef() const noexcept
    {
        return _sceneLightsRef;
    }
    // §P5.5 C — number of lights with castShadow=true (max 8).
    // 0 ⇒ pre-C byte-equivalent single caster path. Consumed by
    // LightingPass to upload `perLightShadowCount.x = N`.
    uint32_t perLightShadowCount() const noexcept
    {
        return _perLightShadowCount;
    }
    // §P5.5 C — atlas sub-rects in UV [0,1] (consumed by
    // LightingPass to upload `shadowAtlasRects[8]`).
    const float* atlasSubRects() const noexcept
    {
        return &_atlasLayout.subRects[0][0];
    }
    // §P5.5 C — per-slot light-space VP matrices (col-major
    // float[16] each, 8 slots). Consumed by LightingPass to upload
    // `lightViewProjs[8]`. Slots >= perLightShadowCount() are
    // identity (no-op).
    const float* atlasLightViewProjsColumnMajor() const noexcept
    {
        return &_atlasLightViewProjsCol[0][0];
    }
    // §P5.5 C — per-slot shadow bias (consumed by LightingPass to
    // upload `shadowBiases[8]`). 0 ⇒ use the global `shadowBias`
    // uniform (RenderPass::tryBindShadowSampler's old contract).
    const float* atlasShadowBiases() const noexcept
    {
        return _atlasShadowBiases;
    }
    // §P5.5 C — atlas pixel rect for a given slot (consumed by
    // ShadowPass internally to drive BGFXAdapter::setScissorRect).
    ShadowAtlasPixelRect slotPixelRect(uint32_t slot) const noexcept
    {
        return shadowAtlasSlotPixelRect(_atlasLayout, slot);
    }

    // §P5.5 C — atlas config setter (currently only used by tests
    // for sizing verification). Defaults = kDefaultAtlasSize + 8.
    void setAtlasConfig(const ShadowAtlasConfig& cfg) noexcept
    {
        _atlasConfig = cfg;
        _atlasLayout = computeShadowAtlasLayout(cfg);
    }
    const ShadowAtlasConfig& atlasConfig() const noexcept { return _atlasConfig; }
    const ShadowAtlasLayout& atlasLayout() const noexcept { return _atlasLayout; }

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

    // §P5.5 C (2026-07-23) — atlas + per-light shadow producer state.
    ShadowAtlasConfig          _atlasConfig{kDefaultAtlasSize,
                                            kShadowAtlasMaxSlots};
    ShadowAtlasLayout          _atlasLayout =
        computeShadowAtlasLayout(_atlasConfig);
    const ayt::render::SceneLights* _sceneLightsRef = nullptr;
    uint32_t                   _perLightShadowCount = 0;
    // Per-slot LVP matrices (col-major float[16]). Slot i is
    // populated when lights[i].castShadow=true; identity otherwise.
    ayt::math::Float4x4        _atlasLightViewProjs[kShadowAtlasMaxSlots]
        {ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity()};
    float                      _atlasLightViewProjsCol[kShadowAtlasMaxSlots][16] = {
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
        {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    };
    // Per-slot shadow bias override (0 ⇒ use global).
    float                      _atlasShadowBiases[kShadowAtlasMaxSlots] = {
        0,0,0,0, 0,0,0,0
    };
};

} // namespace ayt::render::detail
