#pragma once

// S1b BloomBlurPass (short-term-plan §S1 sub-cut 2 of 4, 2026-07-23)
// — half-resolution separable-Gaussian blur ping-pong pass inserted
// AFTER BloomExtract and BEFORE PostProcess on BOTH Forward +
// Deferred default pipelines.
//
// Reads the bright-extract FBO from the BloomExtractPass producer
// (via `ctx.bloomExtractPass->halfResFbo()`) and ping-pongs two of
// its own halfW × halfH FBOs:
//   Pass A (horizontal, view 12): _pingFbo  = blur_h(_sourceFbo)
//   Pass B (vertical,   view 13): _pongFbo  = blur_v(_pingFbo)
// Final blurred result lives in _pongFbo (the second ping-pong
// target). S1c Final-PP composite will sample `_pongFbo` as the
// actual bloom contribution (replacing the pre-S1 fake
// `raw + raw*bloomStrength` PostProcessPass shader hack).
//
// Why now: the short-term-plan §S1 cutsheet mandates a true
// half-resolution bloom chain. S1a shipped the bright-extract
// shader + wire; S1b ships the blur (separable Gaussian = 1
// horizontal submit + 1 vertical submit = 9 taps total instead of
// 81 for a naive 9×9 — Karis's classic 2-pass separable blur).
// S1c will composite the result; S1d is the Editor knob.
//
// View id allocation (cutsheet §S1 §1 + §5.1 spirit): composite
// view table 0..11 + UI=11 already taken by Shadow(1)/Shadow-
// Resolve(2)/FO(0)/Trans(3,9)/PP(4,10)/Skybox(6)/GBuffer(7)/
// Lighting(8)/BloomExtract(5)/UI(11). Views 12 + 13 are the only
// contiguous pair of unused slots before S1b — we claim them for
// BloomBlur H + V. Each ping-pong submit uses its own view id so
// bgfx keeps the FBO + VP bindings independent (cutting
// `setViewFrameBuffer` re-bind between the two submits).
//
// Cutsheet §S1 implementation constraints (mirror S1a):
//   - FBO 生命周期：ensure(w/2, h/2), resize-on-viewport-change.
//     Both ping-pong FBOs live here; size tracked identically.
//   - viewId：紧挨现有 PP blit (12 + 13) — 文档写死占用表，勿与
//     Shadow/GBuffer 撞。永不与其他 pass 重叠。
//   - 一律 `uniform vec4` + cache key bump.
//   - 不要引入资源图、不要自动 alias.
//
// Phoskia uniform gates (lessons §3.1): all scalar knobs uploaded
// as `uniform vec4` with .x carry — bgfx uniform slot is Vec4
// (4-byte float upload is UB), Phoskia `vec4 + .x` is the safe
// contract.
//
// Noop-backend safety: dual guard `!isInitialized() ||
// isNoopBackend()` (mirror S1a BloomExtractPass + PostProcessPass +
// ShadowPass + GBufferPass + LightingPass + SkyboxPass). When
// either guard fires, the entire execute() body short-circuits to
// 0 draws + 0 side effects. Headless tests rely on this. S1b also
// gates on `ctx.bloomExtractPass == nullptr` (producer absent —
// Forward custom desc that omits BloomExtract) and on the
// post-shader acquire failure (Phoskia parser may fail without
// shaderc) — if the program never acquired, execute() still
// returns 0 instead of crashing (matches BloomExtractPass::execute
// contract).
//
// K2 invariants (must survive S1c Final-PP composite + S1d Editor
// knob additions):
//   1. `ctx.bloomExtractPass` invalid OR producer FBO invalid ⇒
//      execute() returns 0 + no ping-pong FBO created (K1 #2
//      propagated; producer-absent = no work).
//   2. Noop backend ⇒ execute() returns 0 + no FBO created
//      (BGFXAdapter gates FBO create on isInitialized; mirrors S1a).
//   3. half-res size = identical to BloomExtract's (W+1)/2 ×
//      (H+1)/2 — ensure on resize. Mirrored from
//      `ctx.bloomExtractPass->halfWidth()/halfHeight()` each frame
//      (no separate setOutputSize host call needed — the
//      producer is the source of truth).
//   4. S1b doesn't touch FrameContext / RenderScene / RenderPass
//      signature (no field additions to FrameContext; no execute
//      signature change). Uses `ctx.bloomExtractPass` borrowed
//      pointer (single field append-only, same lifetime contract
//      as the other borrowed ptrs).
//   5. ABI: append-only — RenderPassSlot::BloomBlur = 9 (was 8
//      after S1a; BloomExtract was the previous append). No
//      existing enum value reorders.
//   6. View id table: 12 + 13 reserved FOREVER for this pass. S1c
//      Final PP composite (the upcoming sub-cut 3) does NOT get
//      its own view id — it samples the existing kBlitViewId=4
//      (Forward) / kDeferredBlitViewId=10 (Deferred) by binding
//      _pongFbo as an additional sampler. Forward-compat: any
//      future pass that needs a new view id MUST pick from ≥14.

#include "AYShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class BloomBlurPass : public RenderPass {
public:
    // §S1b (2026-07-23) — composite view map lock. Must differ from
    // every other slot (FO=0, ShadowC=1, ShadowR=2, Trans=3, PP=4,
    // BloomExtract=5, Skybox=6, GBuffer=7, Lighting=8,
    // Trans-deferred=9, PP-deferred=10, UI=11). Views 12 + 13
    // were the only contiguous pair of free slots before S1b.
    static constexpr uint8_t kBloomBlurHorizontalViewId = 12;
    static constexpr uint8_t kBloomBlurVerticalViewId   = 13;

    BloomBlurPass() = default;
    // Mirror S1a BloomExtractPass + PostProcessPass: dtor does NOT
    // touch bgfx handles. RenderPass base has no BGFXAdapter
    // reference (passes are adapter-agnostic); BGFXAdapter::
    // shutdown() invalidates all handles globally. For mid-frame
    // adapter teardown, call destroyResources() explicitly first.
    ~BloomBlurPass() override = default;

    std::string_view name() const override { return "BloomBlur"; }

    uint32_t execute(PassExecContext& ctx) override;

    // §S1b (2026-07-23) — R5+ mirror. Query whether the pass has
    // real FBOs + program wired. Useful for hosts that want to
    // skip the slot via setEnabled(false) when the bloom pipeline
    // cannot be created (e.g. backend was initialized but the
    // Phoskia program is not in the pool). Today "ready" once
    // execute() has built both ping-pong FBOs at least once.
    bool isReady() const noexcept {
        return bgfx::isValid(_pingFbo) && bgfx::isValid(_pongFbo);
    }

    // §S1c Final-PP composite entry point. The Final pass samples
    // _pongFbo (the vertically-blurred result) as the actual
    // bloom contribution. Returns BGFX_INVALID_HANDLE when the
    // FBOs haven't been ensured yet (first frame race) — caller
    // gates on bgfx::isValid.
    bgfx::FrameBufferHandle pingFbo() const noexcept { return _pingFbo; }
    bgfx::FrameBufferHandle pongFbo() const noexcept { return _pongFbo; }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). Mirror S1a BloomExtractPass::
    // destroyResources contract. Idempotent (BGFXAdapter::destroy
    // on invalid handle is a no-op).
    void destroyResources(BGFXAdapter& adapter);

private:
    // §S1b (2026-07-23) — lazy ping-pong FBOs. Both half-resolution
    // RGBA8, no depth (mirror S1a BloomExtractPass::_fbo). BGFXAdapter
    // owns the bgfx handles; this class owns the cache + size
    // tracking + destroy decision. Sized identically to the
    // BloomExtract producer (read each frame from
    // `ctx.bloomExtractPass->halfWidth()/halfHeight()`).
    bgfx::FrameBufferHandle    _pingFbo    = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle    _pongFbo    = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;
    // Cached source attachment — read each frame from the
    // BloomExtract producer's RT0. Not owned here; mirrors
    // ShadowPass::shadowMap depth attachment pattern.
    bgfx::TextureHandle        _sourceRt   = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle        _pingRt     = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    uint16_t                   _fboWidth   = 0;
    uint16_t                   _fboHeight  = 0;

    // §S1b (2026-07-23) — Phoskia program for the separable
    // Gaussian blur effect (single program, branched via uniform
    // `direction` = (1,0) for horizontal, (0,1) for vertical).
    // Acquired lazily on first execute() after adapter init.
    // Acquire may fail (shaderc missing on CI / disk cache miss
    // + parse error); in that case isReady() stays false and
    // execute() degrades to "early-return 0" — visually identical
    // to bloomStrength=0 host (S1a K1 #1 propagated).
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on first acquire; InvalidBinding
    // means "not yet resolved / acquire failed".
    ayt::shader::BindingId      _uDirection = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uTexelSize = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSource    = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every
    // frame (same stutter source PostProcessPass + S1a
    // BloomExtractPass mitigated).
    bool                        _programAcquireFailed = false;

    // R5+ helpers — no-ops on the Noop backend (BGFXAdapter
    // gates on isInitialized()), so the headless test path runs
    // clean.
    void ensurePingPongFbos(BGFXAdapter& adapter, uint16_t halfW, uint16_t halfH);
    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

// §S1b (2026-07-23) — Bug fix #3 mirror (see LightingPass.h:177-189
// for the originating pattern in §P5.5 B, mirrored by S1a
// BloomExtractPass). Externalize the cache-key literal so unit
// tests can include this header and compare their mirror against
// the live literal. Pre-S1b, kBloomBlurCacheKey was a `.cpp`
// static (not addressable from outside), so tests fell back to
// string self-comparison ("mine == mine") and the drift detection
// was a no-op (false green — same drift trap that bit Test_B5 in
// §P5.5 B). The extern declaration gives every test a single
// source of truth; drift = test fails immediately.
//
// Naming: `kBloomBlurCacheKeyCStr` (CStr suffix = "raw C-string"
// per the AY naming rules). The actual string literal lives in
// BloomBlurPass.cpp as the canonical definition.
extern const char* const kBloomBlurCacheKeyCStr;

} // namespace ayt::render::detail