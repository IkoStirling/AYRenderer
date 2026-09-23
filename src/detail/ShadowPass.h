#pragma once

#include "AYRenderer/RenderScene.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/ShadowAtlas.h"
#include "detail/ShadowCaster.h"
#include "detail/ShadowMapResources.h"

#include "AYRenderer/ShadowConfig.h"

#include "AYMath/MathTypes.h"

#include <bgfx/bgfx.h>

#include <climits>
#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class ShadowPass : public RenderPass {
public:
    static constexpr uint32_t kDirectionalCascadeCount = 3u;
    static constexpr uint16_t kDefaultShadowMapSize = 2048;
    static constexpr float    kDefaultFrustumRadius = ayt::render::kShadowDefaultFrustumRadius;
    static constexpr float    kShadowNearPlane      = ayt::render::kShadowNearPlane;
    static constexpr float    kShadowFarPlane       = ayt::render::kShadowFarPlane;
    // Composite view map: 1 = caster FBO, 2 = resolve blit (must differ).
    static constexpr uint8_t  kShadowViewId        = 1;
    static constexpr uint8_t  kShadowResolveViewId = 2;
    // Each atlas slot needs its own bgfx view: view state (transform,
    // scissor, framebuffer) is shared by every submit in a view.
    static constexpr uint8_t  kShadowAtlasFirstViewId = 18;
    static constexpr uint8_t  kShadowAtlasViewCount =
        static_cast<uint8_t>(kShadowAtlasMaxSlots);
    // §P5.5 C (2026-07-23) — atlas size when per-light shadow is
    // active (4096×4096 default ⇒ 8 sub-rects of 1024×2048 each in
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
        // A disabled pass may still own a valid map from the previous frame.
        // Do not let receivers sample that stale map: disabled means fully lit.
        return isEnabled() && _producedThisFrame
            && _mapResources.hasSampleableShadow();
    }

    bool lastBlitOk() const noexcept { return _mapResources.lastBlitOk(); }

    void setShadowMapSize(uint16_t size) noexcept
    {
        _requestedSize = size;
        _sampleMapSize = 0;
        _producedThisFrame = false;
    }
    uint16_t shadowMapSize() const noexcept { return _requestedSize; }
    uint16_t shadowSampleMapSize() const noexcept
    {
        return _sampleMapSize > 0 ? _sampleMapSize : _requestedSize;
    }

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
    // execute() publishes the legacy directional caster as slot 0 with a
    // full-texture sample rect, so consumers use one unified array path.
    void setSceneLightsRef(const ayt::render::SceneLights* lights) noexcept
    {
        _sceneLightsRef = lights;
    }
    const ayt::render::SceneLights* sceneLightsRef() const noexcept
    {
        return _sceneLightsRef;
    }
    // Number of active atlas projections (base lights plus extra directional
    // cascades, max 8). Legacy fallback reports one projection.
    uint32_t perLightShadowCount() const noexcept
    {
        return _perLightShadowCount;
    }
    uint32_t shadowProjectionCount() const noexcept
    {
        return _shadowProjectionCount;
    }
    // §P5.5 C — atlas sub-rects in UV [0,1] (consumed by
    // LightingPass to upload `shadowAtlasRects[8]`).
    const float* atlasSubRects() const noexcept
    {
        return &_atlasLayout.subRects[0][0];
    }
    const float* shadowSampleRects() const noexcept
    {
        return &_shadowSampleRects[0][0];
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
    // Per ordered light: xyz are near/mid/far atlas projection indices and w
    // is cascade count. Spot lights repeat their single projection index.
    const float* shadowLightProjectionSlots() const noexcept
    {
        return &_shadowLightProjectionSlots[0][0];
    }
    // Per ordered light: xyz are positive camera-view split distances.
    const float* shadowCascadeSplits() const noexcept
    {
        return &_shadowCascadeSplits[0][0];
    }
    const float* shadowCameraForward() const noexcept
    {
        return _shadowCameraForward;
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
    uint16_t                   _sampleMapSize = 0;
    bool                       _pcfEnabled    = true;
    bool                       _producedThisFrame = false;

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
    float                      _shadowSampleRects[kShadowAtlasMaxSlots][4] = {
        {0,0,1,1}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0},
        {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}
    };
    // Per-slot matrices. bgfx consumes View + Projection separately while
    // LightingPass consumes the composed ViewProjection.
    ayt::math::Float4x4        _atlasLightViews[kShadowAtlasMaxSlots]
        {ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity()};
    ayt::math::Float4x4        _atlasLightProjs[kShadowAtlasMaxSlots]
        {ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity()};
    ayt::math::Float4x4        _atlasLightViewProjs[kShadowAtlasMaxSlots]
        {ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity(),
         ayt::math::Float4x4::identity()};
    float                      _atlasLightViewsCol[kShadowAtlasMaxSlots][16] = {};
    float                      _atlasLightProjsCol[kShadowAtlasMaxSlots][16] = {};
    // §P4 M9 (2026-08-24) — identity-col-major default for the
    // col-major per-slot LVP array. The pattern "(c % 5 == 0) ? 1 : 0"
    // (= 1 at indices 0,5,10,15; 0 elsewhere) is the row-major
    // identity written in col-major storage order. LightingPass's
    // "slot i is no-op when LVP is identity" gate reads `LVP == I`
    // (exact bit compare); this default keeps slots >= activeCount
    // bit-identical to I so the gate evaluates to "no shadow" without
    // any explicit `if (slot >= count) skip` branch on the consumer
    // side. Test_ShadowAtlasLayout (T6) pins this contract.
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
    float                      _shadowLightProjectionSlots[kMaxSceneLights][4] = {};
    float                      _shadowCascadeSplits[kMaxSceneLights][4] = {};
    float                      _shadowCameraForward[4] = {0,0,1,0};
    // Keep all post-P5.5 state appended after the original object layout.
    // This protects incremental MSVC builds from mixing an older constructor
    // object with a newer accessor while the editor executable is still open.
    uint32_t                   _shadowProjectionCount = 0;
};

} // namespace ayt::render::detail
