#pragma once

// S1b BloomBlurPass (short-term-plan §S1 sub-cut 2 of 4, 2026-07-23)
// — half-resolution separable-Gaussian blur ping-pong pass inserted
// AFTER BloomExtract and BEFORE PostProcess on BOTH Forward +
// Deferred default pipelines.
//
// Reads the bright-extract FBO via the FrameGraph (BloomBright) and
// ping-pongs into two of its own halfW × halfH FBOs (BloomBlurA →
// BloomBlurB):
//   Pass A (horizontal, view 11): BloomBlurA = blur_h(BloomBright)
//   Pass B (vertical,   view 12): BloomBlurB = blur_v(BloomBlurA)
// Final blurred result lives in BloomBlurB (the second ping-pong
// target). S1c Final-PP composite will sample BloomBlurB as the
// actual bloom contribution (replacing the pre-S1 fake
// `raw + raw*bloomStrength` PostProcessPass shader hack).
//
// §F3 (2026-07-24, mid-term FG MVP sub-cut 3) — migration:
//   - Pre-F3: BloomBlurPass owns `_pingFbo` / `_pongFbo` (header
//     rows 144-145); execute() calls ensurePingPongFbos() each frame.
//   - F3:     BloomBlurPass no longer owns either FBO. Both targets
//     are now owned by the FrameGraph (BloomBlurA / BloomBlurB —
//     two distinct FgResourceId entries; aliasing is forbidden by
//     design — see F6 alias decision + cutsheet §4 "BloomBlur A/B
//     显式禁止 alias"). execute() reads them via
//     `ctx.frameGraph->resolvePingPong(BloomBlurA, BloomBlurB)`.
//
// Why F3 ships the migration now: cutsheet §7 升条件
// (≥2 fullscreen passes, ping-pong boilerplate 三次, 关效果即不
// 分配 RT) all met. F3 removes the second hand-written ping-pong
// boilerplate (after S1b's original `ensurePingPongFbos`) — F4 will
// remove DepthHaze's, and F6 will centralize the resize path.
//
// View id allocation: BloomExtract=10 → BlurH=11 → BlurV=12 →
// DepthHaze=13 → PostProcess=14 → UI=255 (fixed high slot). View
// ids stay on the Pass — FG never allocates them.
//
// Cutsheet §S1 implementation constraints (mirror S1a):
//   - FBO 生命周期：ensure(w/2, h/2), resize-on-viewport-change.
//     F3: lifecycle moves to FrameGraph; FG calls
//     BGFXAdapter::createFrameBuffer lazily on first resolvePingPong
//     (deferred to F6's physical-creation cut). Today (F3) the
//     resolvePingPong path still returns invalid FG handles on
//     Noop / uninitialized adapters, so this pass degrades to
//     "0 draws" — visually identical to pre-F3 host behavior with
//     bloomStrength=0.
//   - viewId：紧挨现有 PP blit (11 + 12) — 文档写死占用表，勿与
//     Shadow/GBuffer 撞。永不与其他 pass 重叠。
//   - 一律 `uniform vec4` + cache key bump.
//   - 不要引入资源图、不要自动 alias (F6 才做 alias decision)。
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
// 0 draws + 0 side effects. F3 adds a THIRD early-return on
// `ctx.frameGraph == nullptr` (legacy caller pattern: pre-F3 test
// sites that never wired the FrameGraph) — same byte-equivalent
// "no bloom" path.
//
// K2 invariants (must survive S1c Final-PP composite + S1d Editor
// knob additions):
//   1. `ctx.frameGraph == nullptr` OR producer (BloomBright) not
//      live OR BloomBlurA/B not live ⇒ execute() returns 0 + no
//      FBO created (F3 owns no FBO; the FG lazily creates them
//      only when resolvePingPong succeeds).
//   2. Noop backend ⇒ execute() returns 0 + no FBO created
//      (BGFXAdapter gates FBO create on isInitialized; mirrors S1a).
//   3. half-res size = identical to BloomExtract's (W+1)/2 ×
//      (H+1)/2 (mirror BloomExtract). F3 source-of-truth is the
//      FrameGraph live-physical size; pre-F6 (physical creation
//      deferred) the pass computes halfW/halfH locally exactly
//      like pre-F3.
//   4. F3 doesn't touch FrameContext / RenderScene signature.
//      PassExecContext got one appended field (frameGraph) back in
//      F2; BloomBlurPass consumes it. BloomBlurPass's own state
//      lost `_pingFbo`/`_pongFbo`/`_fboWidth`/`_fboHeight` (those
//      live on FG now).
//   5. ABI: append-only — RenderPassSlot::BloomBlur = 9 (unchanged
//      from S1b). No existing enum value reorders.
//   6. View id table: Extract=10, BlurH=11, BlurV=12, DepthHaze=13,
//      PP=14, UI=255. Future passes that need a new view id MUST
//      pick from ≥16.
//   7. (NEW in F3) FG owns BloomBlurA and BloomBlurB; aliasing
//      between the two is forbidden by design (their lifecycles
//      overlap: H writes A while V reads A and writes B). F6's
//      alias-decision pass MUST keep them in separate physical
//      FBOs even when their FgTextureDesc matches.

#include "AYShader/ShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class BloomBlurPass : public RenderPass {
public:
    // §S1b view map: after BloomExtract=10, before DepthHaze=13.
    // UI is fixed at 255 (not adjacent — leave Post headroom).
    static constexpr uint8_t kBloomBlurHorizontalViewId = 11;
    static constexpr uint8_t kBloomBlurVerticalViewId   = 12;

    BloomBlurPass() = default;
    // Mirror S1a BloomExtractPass + PostProcessPass: dtor does NOT
    // touch bgfx handles. RenderPass base has no BGFXAdapter
    // reference (passes are adapter-agnostic); BGFXAdapter::
    // shutdown() invalidates all handles globally. For mid-frame
    // adapter teardown, call destroyResources() explicitly first.
    ~BloomBlurPass() override = default;

    std::string_view name() const override { return "BloomBlur"; }

    uint32_t execute(PassExecContext& ctx) override;

    // §F3 (2026-07-24) — F3 ships with FG 物理创建延后到 F6。
    // isReady() reflects "Are BloomBlurA AND BloomBlurB physically
    // live + valid + paired?" Today (F3) those resolve()s return
    // invalid in the FG-skeleton phase, so isReady()恒 false (same
    // shape as BloomExtractPass F2 isReady). F6 will replace this
    // with a real FG-backed readiness probe.
    bool isReady() const noexcept { return false; }

    // §F3 deprecated (2026-07-24) — ping/pong FBO getters used to
    // hand out the bloom chain's vertical result to PostProcessPass
    // (S1c S4c pattern). After F3 / F5 migration, the consumer
    // pathway goes through `ctx.frameGraph->resolveSemantic(
    // FgSemantic::BloomSource)` (F5) — both pingFbo() and pongFbo()
    // become legacy shims that return BGFX_INVALID_HANDLE, and F5
    // removes them entirely. Defensive keep: any pre-F3 site that
    // still calls them gets a sentinel return instead of dangling
    // members; the S1c-S4c consumer chain in PostProcessPass still
    // uses `ctx.bloomBlurPass` borrowed ptr, but the getHalfResFbo()
    // reads now go through FG.
    //
    // Lifetime contract (legacy): the pre-F3 callers handed these
    // handles to PostProcessPass S1c as `bloomTexture` and to
    // DepthHazePass as `halfResFbo()`. F5 replaces that with FG
    // resolveSemantic; F5 ship removes these getters.
    bgfx::FrameBufferHandle pingFbo() const noexcept {
        return BGFX_INVALID_HANDLE;
    }
    bgfx::FrameBufferHandle pongFbo() const noexcept {
        return BGFX_INVALID_HANDLE;
    }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). §F3 — F3 ships with no FBO to release;
    // destroyResources only releases the Phoskia program + the
    // fullscreen VB/IB. FG-owned RTs (BloomBlurA / BloomBlurB) are
    // released by FrameGraph::shutdown / FrameGraph::resize (the
    // Renderer's Impl shutdown path calls fg.shutdown()).
    void destroyResources(BGFXAdapter& adapter);

private:
    // §F3 (2026-07-24) — fields removed in F3:
    //   `_pingFbo`          → FrameGraph.BloomBlurA
    //   `_pongFbo`          → FrameGraph.BloomBlurB
    //   `_fboWidth`/`_fboHeight` → FG physical size (deferred to F6)
    //   `_sourceRt` / `_pingRt` → still kept locally as transient
    //      cache of `adapter.getFboAttachment(handle, 0)` for the
    //      current frame's source / ping attachments (cheap lazy
    //      refresh; mirrors pre-F3 behavior).
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle        _sourceRt     = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle        _pingRt       = bgfx::TextureHandle{BGFX_INVALID_HANDLE};

    // §S1b (2026-07-23) — Phoskia program for the separable
    // Gaussian blur effect (single program, branched via uniform
    // `direction` = (1,0) for horizontal, (0,1) for vertical).
    // Acquired lazily on first execute() after adapter init.
    // Acquire may fail (shaderc missing on CI / disk cache miss
    // + parse error); in that case isReady() stays false and
    // execute() degrades to "early-return 0" — visually identical
    // to bloomStrength=0 host (S1a K1 #1 propagated).
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on the first acquire; InvalidBinding
    // means "not yet resolved / acquire failed".
    ayt::shader::BindingId      _uDirection = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uTexelSize = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSource    = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every
    // frame (same stutter source PostProcessPass + S1a
    // BloomExtractPass mitigated).
    bool                        _programAcquireFailed = false;

    // R5+ helpers — VB/IB + program acquisition only (FBO ensure
    // removed in F3; FG owns both ping-pong RTs now).
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
