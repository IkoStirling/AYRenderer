#pragma once

// Deferred geometry producer. Owns a four-color MRT plus depth:
//   RT0 RGBA8   albedo.rgb, metallic.a
//   RT1 RGBA8   encoded world normal.rgb, roughness.a
//   RT2 RGBA16F world position.xyz, packed(AO, material model).a
//   RT3 RGBA8   emissive.rgb, independent geometry coverage.a
//   D24S8       opaque/cutout depth
// Consumers borrow attachment handles through PassExecContext. Allocation
// readiness and current-frame production are separate contracts so stale
// attachments never masquerade as fresh pass output.

#include "detail/BGFXAdapter.h"
#include "detail/GBufferLayout.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <AYShader/ShaderResource.h>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class GBufferPass : public RenderPass {
public:
    // View 7 is the fixed geometry/MRT producer view.
    static constexpr uint8_t  kGBufferViewId   = 7;
    static constexpr uint8_t  kGBufferAttachmentCount =
        GBufferLayout::kColorAttachmentCount;
    static constexpr uint16_t kGBufferDefaultSize = 1280;

    GBufferPass() = default;
    ~GBufferPass() override;

    std::string_view name() const override { return "GBuffer"; }

    uint32_t execute(PassExecContext& ctx) override;

    // §P5 B4a (2026-07-22) �?fix B2 known-bug `&& false`. Now reads:
    // "FBO handle index is valid (�?UINT16_MAX)". Default-constructed
    // passes return false (BGFX_INVALID_HANDLE = UINT16_MAX), so this
    // is the correct inverse of `_gbufferFbo.isValid()` semantics.
    bool isReady() const noexcept {
        return _gbufferFbo.idx != UINT16_MAX && hasValidAttachments();
    }

    // `isReady()` describes allocated resources. `producedThisFrame()` is the
    // stronger consumer contract: it becomes true only after execute() has
    // completed normally and queued the MRT clear/draw work for this frame.
    // Renderer::render() calls setGbufferSize() every frame (including 0x0),
    // which resets this bit before pipeline dispatch.
    bool producedThisFrame() const noexcept { return _producedThisFrame; }
    void resetFrameState() noexcept { _producedThisFrame = false; }
    bool hasValidAttachments() const noexcept {
        return bgfx::isValid(_gbufferAlbedoRt)
            && bgfx::isValid(_gbufferNormalRt)
            && bgfx::isValid(_gbufferWorldPositionRt)
            && bgfx::isValid(_gbufferMaterialRt)
            && bgfx::isValid(_gbufferDepthRt);
    }

    // Borrowed attachment accessors. They remain invalid until ensure()
    // successfully creates and caches the complete framebuffer.
    bgfx::FrameBufferHandle gbufferFbo() const noexcept { return _gbufferFbo; }
    bgfx::TextureHandle     gbufferAlbedoRt() const noexcept { return _gbufferAlbedoRt; }
    bgfx::TextureHandle     gbufferNormalRt() const noexcept { return _gbufferNormalRt; }
    bgfx::TextureHandle     gbufferWorldPositionRt() const noexcept {
        return _gbufferWorldPositionRt;
    }
    // Compatibility alias for pre-contract callers. RT2 has contained world
    // position—not motion—since the deferred shadow path was introduced.
    bgfx::TextureHandle     gbufferMotionRt() const noexcept {
        return gbufferWorldPositionRt();
    }
    bgfx::TextureHandle     gbufferMaterialRt() const noexcept { return _gbufferMaterialRt; }
    // The depth attachment is exposed for debug and future screen-space
    // consumers. Lighting currently reads RT2 world position directly.
    bgfx::TextureHandle     gbufferDepthRt()  const noexcept { return _gbufferDepthRt; }
    uint16_t                gbufferWidth() const noexcept { return _gbufferW; }
    uint16_t                gbufferHeight() const noexcept { return _gbufferH; }

    // §P5 B4a (2026-07-22) �?build stamp pointer (mirror
    // ShadowMapResources.h:55 `const char* _buildStamp` shape).
    // Comparison via pointer equality (NOT string compare) �?callers
    // MUST pass a string literal with stable lifetime. Default `""`
    // means "never ensured"; first ensure() pins the literal.
    const char*             buildStamp()      const noexcept { return _buildStamp; }

    // B4 will move these into an internal `ensure()` like ShadowPass
    // does for `_mapResources.ensure()`. B2 leaves them as public
    // stubs so external resizing code can be wired without an ABI
    // churn when B4 lands.
    void setGbufferSize(uint16_t width, uint16_t height) noexcept;
    void destroyResources(BGFXAdapter& adapter);

    // §P5 B4b (2026-07-22) �?lazy Phoskia GBuffer VS/FS acquire.
    // Mirrors ShadowCaster::ensureProgram shape (public). Stamp-
    // checked `static const char* s_acquiredCacheKey != kGBufferCacheKey`
    // invalidates cached program when the literal bumps. Returns
    // silently when the compile fails (sets _acquireFailed=true).
    void ensureProgram(ayt::shader::ShaderResourcePool& pool);
    bool isProgramReady() const noexcept;

    // Host-side previous-frame transform plumbing is retained for a future
    // velocity attachment. Renderer::render() updates it once per frame;
    // the current four-target GBuffer does not write motion vectors.
    //
    // CPU-side: execute() builds `prevViewProj = projection * view`
    // (P×V same-order as `setViewTransform` + `viewProjectionMatrix`
    // builtin ordering �?mirror `docs/pass-lessons-from-shadow.md`
    // §3.1 warning). The host just hands the raw pieces.
    //
    // Identity remains the deterministic first-frame seed.
    void setPrevViewProj(const ayt::math::Float4x4& view,
                         const ayt::math::Float4x4& projection) noexcept;

    // §P5 B4c (2026-07-22) �?read-only accessors for tests (mirror
    // the read-back shape Test_B2_GBufferPass uses for FBO handles).
    // The matrices the host pushed, NOT the multiplied prevViewProj
    // �?tests can verify the round-trip without depending on
    // execute() running.
    ayt::math::Float4x4 prevView()       const noexcept { return _prevView; }
    ayt::math::Float4x4 prevProjection() const noexcept { return _prevProjection; }

private:
    // §P5 B4a (2026-07-22) �?add depth RT handle + build stamp pointer.
    // _gbufferDepthRt mirrors _gbufferAlbedoRt/_gbufferNormalRt/
    // _gbufferWorldPositionRt shape (BGFX_INVALID_HANDLE default).
    bgfx::TextureHandle _gbufferDepthRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    const char*         _buildStamp      = "";

    bgfx::FrameBufferHandle _gbufferFbo       = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle     _gbufferAlbedoRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle     _gbufferNormalRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle     _gbufferWorldPositionRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle     _gbufferMaterialRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    // Requested panel size (setGbufferSize). Compared against
    // _allocatedW/H in ensure() �?must NOT reuse the request fields
    // for the cache check or a resize that only updates the request
    // will skip FBO rebuild (deferred viewport tears / jagged edges).
    uint16_t                _gbufferW         = 0;
    uint16_t                _gbufferH         = 0;
    uint16_t                _allocatedW       = 0;
    uint16_t                _allocatedH       = 0;

    // §P5 B4a (2026-07-22) �?internal ensure (mirror
    // ShadowMapResources::ensure shape). Calls
    // adapter.createGbufferFrameBuffer + caches 4 attachments.
    // Public surface: 0 (private).
    void ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height);
    void cacheAttachments(BGFXAdapter& adapter);

    // §P5 B4b (2026-07-22) �?Phoskia GBuffer VS/FS program handle
    // (mirror ShadowCaster::_program).
    ayt::shader::ShaderResource _program;
    // Imported 3D cutouts retain their existing raw .sc compatibility
    // sibling. WorldLit2D uses the current Phoskia fragment-discard path.
    ayt::shader::ShaderResource _alphaCutoutProgram;
    // Position+UV-only program for WorldLit2D quads. It derives the local
    // +Z normal/+X tangent instead of requiring a 3D mesh vertex layout.
    ayt::shader::ShaderResource _worldLit2DProgram;
    bool _acquireFailed = false;
    bool _alphaCutoutAcquireFailed = false;
    bool _worldLit2DAcquireFailed = false;
    bool _producedThisFrame = false;

    // Previous-frame cache reserved for a future dedicated velocity target.
    ayt::math::Float4x4 _prevView       = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 _prevProjection = ayt::math::Float4x4::identity();
};

// Live cache-key / build-stamp for unit tests (mirror LightingPass).
extern const char* const kGBufferCacheKeyCStr;
extern const char* const kGBufferBuildStampCStr;
extern const char* const kGBufferPhoskiaSourceCStr;
extern const char* const kWorldLit2DGBufferPhoskiaSourceCStr;

} // namespace ayt::render::detail
