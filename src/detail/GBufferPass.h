#pragma once

// §P5 B2 (2026-07-22) — GBufferPass empty shell.
//
// Mirrors ShadowPass's plumbing shape (PR-F2, 2026-07-21): a derived
// RenderPass that owns its producer state (the GBuffer MRT attachments)
// and exposes them via non-owning pointers / accessors that downstream
// consumers (the B5 LightingPass, then B7+ multi-light consumers)
// read through `PassExecContext::gbufferPass`.
//
// This B2 commit ships the SHELL ONLY:
//   - Class skeleton + RenderPass base contract (name + execute)
//   - `isReady() = false` always (no FBO, no program, no allocator)
//   - `execute()` Noop-gates on adapter state and returns 0 draws
//   - Stub accessors return BGFX_INVALID_HANDLE / 0 / 0 so consumers
//     that *compile-link* against this header can still call them
//     and see the expected "no work" shape.
//   - `destroyResources()` is a clean no-op (mirrors ShadowPass shape)
//
// Real GPU MRT + Phoskia GBuffer VS/FS + view-id 7 + R8G8B8A8 +
// D24S8 depth attachment land in B4. Shadowing of the BGFXAdapter
// `createGbufferFrameBuffer` MRT helper also lands in B4 (per
// docs/pass-lessons-from-deferred.md §5.2). Real LightingPass that
// binds the GBuffer as its input lands in B5.
//
// §5.3 red lines we still respect:
//   - No FrameContext writeback of gbuffer RTs (would force const→
//     non-const and re-trigger the §5.5 PR-F1' FrameContext ABI
//     accident). Consumers read via PassExecContext::gbufferPass.
//   - No RenderScene::Light struct added.
//   - No execute(PassExecContext&) signature change on RenderPass
//     base (1 borrowed pointer field, default-init null, fully
//     additive — same shape as PR-F2's shadowPass field).
//
// Why empty shell ships first:
//   B0 / B0.5 docs proved the cutsheet boundary. B1 wired the
//   RenderPath enum + `RenderPipelineDesc::path` plumbing (808/808
//   stable). B2 wires the GBufferPass *plumbing* without GPU so
//   B4 / B5 can land as small, bisectable cuts instead of one
//   big-bang PR. The shadow path was done identically (PR-F1'
//   plumbing-only ShadowPass commit → PR-F2 shadowPass pointer
//   commit → PR-F3 caster state commit → live shadow ship later).
//
// Lifetime: GBufferPass instances are owned by RenderPipeline via
// unique_ptr (same as ShadowPass). The borrowed pointer in
// PassExecContext stays valid for the duration of executeAll().
// Across frames, the pointer remains valid because the pipeline
// outlives every render() call.

#include "detail/BGFXAdapter.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <AYShaderResource.h>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class GBufferPass : public RenderPass {
public:
    // §P5 B4 lands real MRT — RT0 albedo RGBA8, RT1 normal RGBA8,
    // RT2 motion RGBA8, depth D24S8 hardware. Until then we use the
    // bgfx-default invalid handle so anyone who calls `gbufferFbo()`
    // gets a safe "no GBuffer RT this frame" signal (same shape as
    // ShadowPass::shadowFbo() on Noop).
    //
    // View-id allocation: lock plan per docs/pass-lessons-from-
    // deferred.md §5.1 — view 0..6 unchanged (Forward/Transparent/
    // PostProcess/UI), B4 reserves view 7 for GBuffer MRT, B5 reserves
    // view 8 for LightingPass. B2 only pins the symbolic constants
    // (do not use them yet — no GPU work).
    static constexpr uint8_t  kGBufferViewId   = 7;
    static constexpr uint8_t  kGBufferAttachmentCount = 3; // RT0..RT2; depth is RT3 (separate attachment)
    static constexpr uint16_t kGBufferDefaultSize = 1280;

    GBufferPass() = default;
    ~GBufferPass() override;

    std::string_view name() const override { return "GBuffer"; }

    uint32_t execute(PassExecContext& ctx) override;

    // §P5 B4a (2026-07-22) — fix B2 known-bug `&& false`. Now reads:
    // "FBO handle index is valid (≠ UINT16_MAX)". Default-constructed
    // passes return false (BGFX_INVALID_HANDLE = UINT16_MAX), so this
    // is the correct inverse of `_gbufferFbo.isValid()` semantics.
    bool isReady() const noexcept { return _gbufferFbo.idx != UINT16_MAX; }

    // Stub accessors — return invalid handles / identity until B4
    // wires real GPU state. These exist so B5 LightingPass + B7+
    // multi-light consumer code can compile-link against the shell
    // header and run with the expected "no work yet" semantics.
    bgfx::FrameBufferHandle gbufferFbo() const noexcept { return _gbufferFbo; }
    bgfx::TextureHandle     gbufferAlbedoRt() const noexcept { return _gbufferAlbedoRt; }
    bgfx::TextureHandle     gbufferNormalRt() const noexcept { return _gbufferNormalRt; }
    bgfx::TextureHandle     gbufferMotionRt() const noexcept { return _gbufferMotionRt; }
    // §P5 B4a (2026-07-22) — depth attachment accessor. B5 LightingPass
    // doesn't sample depth (samples albedo/normal/motion only) but
    // future B7+ multi-light chain / DebugOverlay GBuffer visualization
    // (cutsheet §6.2) may need linearized depth. Mirror the 3 color
    // accessors' shape — BGFX_INVALID_HANDLE until B4 ensure wires the
    // 4-attach MRT.
    bgfx::TextureHandle     gbufferDepthRt()  const noexcept { return _gbufferDepthRt; }
    uint16_t                gbufferWidth() const noexcept { return _gbufferW; }
    uint16_t                gbufferHeight() const noexcept { return _gbufferH; }

    // §P5 B4a (2026-07-22) — build stamp pointer (mirror
    // ShadowMapResources.h:55 `const char* _buildStamp` shape).
    // Comparison via pointer equality (NOT string compare) — callers
    // MUST pass a string literal with stable lifetime. Default `""`
    // means "never ensured"; first ensure() pins the literal.
    const char*             buildStamp()      const noexcept { return _buildStamp; }

    // B4 will move these into an internal `ensure()` like ShadowPass
    // does for `_mapResources.ensure()`. B2 leaves them as public
    // stubs so external resizing code can be wired without an ABI
    // churn when B4 lands.
    void setGbufferSize(uint16_t width, uint16_t height) noexcept;
    void destroyResources(BGFXAdapter& adapter);

    // §P5 B4b (2026-07-22) — lazy Phoskia GBuffer VS/FS acquire.
    // Mirrors ShadowCaster::ensureProgram shape (public). Stamp-
    // checked `static const char* s_acquiredCacheKey != kGBufferCacheKey`
    // invalidates cached program when the literal bumps. Returns
    // silently when the compile fails (sets _acquireFailed=true).
    void ensureProgram(ayt::shader::ShaderResourcePool& pool);
    bool isProgramReady() const noexcept;

    // §P5 B4c (2026-07-22) — host pushes previous-frame view +
    // projection matrices so the GBuffer FS can compute per-pixel
    // motion vectors against `u_prevViewProj`. Renderer::render()
    // calls this once per frame from the GBuffer slot block (right
    // next to `setGbufferSize`), AFTER `Impl::prevMainView` has
    // been advanced by the previous frame's end-of-frame commit.
    //
    // CPU-side: execute() builds `prevViewProj = projection * view`
    // (P×V same-order as `setViewTransform` + `viewProjectionMatrix`
    // builtin ordering — mirror `docs/pass-lessons-from-shadow.md`
    // §3.1 warning). The host just hands the raw pieces.
    //
    // Identity default: first frame, prev is identity (set by Impl
    // default-init). B4c documents garbage motion on frame 0 as
    // acceptable — B7+ TAA consumer must tolerate one-frame seed
    // noise (TAA is a multi-frame accumulator).
    void setPrevViewProj(const ayt::math::Float4x4& view,
                         const ayt::math::Float4x4& projection) noexcept;

    // §P5 B4c (2026-07-22) — read-only accessors for tests (mirror
    // the read-back shape Test_B2_GBufferPass uses for FBO handles).
    // The matrices the host pushed, NOT the multiplied prevViewProj
    // — tests can verify the round-trip without depending on
    // execute() running.
    ayt::math::Float4x4 prevView()       const noexcept { return _prevView; }
    ayt::math::Float4x4 prevProjection() const noexcept { return _prevProjection; }

private:
    // §P5 B4a (2026-07-22) — add depth RT handle + build stamp pointer.
    // _gbufferDepthRt mirrors _gbufferAlbedoRt/_gbufferNormalRt/
    // _gbufferMotionRt shape (BGFX_INVALID_HANDLE default).
    bgfx::TextureHandle _gbufferDepthRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    const char*         _buildStamp      = "";

    bgfx::FrameBufferHandle _gbufferFbo       = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle     _gbufferAlbedoRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle     _gbufferNormalRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle     _gbufferMotionRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    // Requested panel size (setGbufferSize). Compared against
    // _allocatedW/H in ensure() — must NOT reuse the request fields
    // for the cache check or a resize that only updates the request
    // will skip FBO rebuild (deferred viewport tears / jagged edges).
    uint16_t                _gbufferW         = 0;
    uint16_t                _gbufferH         = 0;
    uint16_t                _allocatedW       = 0;
    uint16_t                _allocatedH       = 0;

    // §P5 B4a (2026-07-22) — internal ensure (mirror
    // ShadowMapResources::ensure shape). Calls
    // adapter.createGbufferFrameBuffer + caches 4 attachments.
    // Public surface: 0 (private).
    void ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height);
    void cacheAttachments(BGFXAdapter& adapter);

    // §P5 B4b (2026-07-22) — Phoskia GBuffer VS/FS program handle
    // (mirror ShadowCaster::_program).
    ayt::shader::ShaderResource _program;
    bool _acquireFailed = false;

    // §P5 B4c (2026-07-22) — previous-frame view/projection cache
    // (mirror ShadowPass's `_lightView/_lightProj` private shape).
    // Default = identity (B4c documented first-frame garbage motion
    // acceptable for B7+ TAA consumer). Host pushes via
    // setPrevViewProj(); execute() multiplies to `_prevViewProj` for
    // uniform upload.
    ayt::math::Float4x4 _prevView       = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 _prevProjection = ayt::math::Float4x4::identity();
};

} // namespace ayt::render::detail