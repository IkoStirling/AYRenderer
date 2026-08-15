#pragma once

// §S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — DepthHazePass
// real implementation. The S4a skeleton cut reserved the
// RenderPassSlot + PassExecContext borrowed ptr + class shape; S4b
// lands the exponential fog shader (主人拍板 B), the half-resolution
// RGBA8 FBO ensure (later deferred to FrameGraph in F4), Deferred
// GBuffer-RT2 worldPos distance (`length(worldPos - camPos)`),
// Forward safe no-haze when gbufferMotionRt is missing, and the
// hazeEnabled/hazeStrength zero-cost gate (K3 invariant #2:
// frame.hazeEnabled=false ⇒ HazeHalf 不 live ⇒ resolve 返 invalid
// ⇒ no FBO allocation; mirrors frame-graph-mvp.md §7 第 3 条).
//
// §F4 (2026-07-24, mid-term FG MVP sub-cut 4) — HazeHalf migration:
// DepthHazePass no longer owns its private `_fbo`; it reads the
// half-resolution FBO through `ctx.frameGraph->resolve(
// FgResourceId::HazeHalf)`. The "关效果即不分配 RT" gate is
// now a compile-time decision (render() builds the FG once per
// frame; bloomStrength=0 / hazeEnabled=false ⇒ the DepthHaze
// pass is not added ⇒ FG compile culls HazeHalf ⇒ resolve returns
// invalid ⇒ pass early-returns 0).
//
// Pipeline position (short-term-plan §S4 决策 2026-07-23):
//   ... Lighting → Transparent → BloomExtract → BloomBlur → DepthHaze → PostProcess
// haze only modifies the raw scene color (exponential
// `1 - exp(-density * dist)`); bloom stays independent (per
// 决策 "haze 只改 raw, bloom 独立"). PostProcessPass S4c will sample
// the haze RT as the `hazeTexture` sampler on the fullscreen
// composite draw — S4c owns that wire (S4c stays).
//
// View id allocation: BloomBlur V=12 → DepthHaze=13 → PostProcess=14.
// Haze MUST sort before Final PP (bgfx ascending view id) so the
// half-res fog RT is filled in the same frame PP samples it.
//
// Phoskia uniform gates (lessons §3.1): all scalars as `uniform vec4`
// with .x carry. hazeColor as vec4 with .xyz used (.w zero pad).
//
// Noop-backend safety: dual guard `!isInitialized() || isNoopBackend()`
// (mirror BloomExtractPass / PostProcessPass / BloomBlurPass /
// ShadowPass / GBufferPass / LightingPass / SkyboxPass). F4 also
// gates on `ctx.frameGraph == nullptr` (legacy caller pattern) and
// on `ctx.frameGraph->resolve(HazeHalf)` returning invalid (which
// subsumes the pre-F4 "hazeEnabled=false" check — the host's knob
// is folded into the render()-central `hazePassEnabled` boolean,
// so by the time execute() runs we either have a valid HazeHalf
// RT or we early-return 0).
//
// K3 invariants (must survive §S4c PostProcessPass haze sampler wire
// + §S4d Editor default-knob polish + §F4 FrameGraph migration):
//   1. hazeStrength <= 0 OR hazeEnabled == false ⇒ DepthHazePass
//      early-returns 0; FG compile culls HazeHalf ⇒ resolve
//      returns invalid ⇒ no FS work. PostProcessPass S4c haze
//      sampler path binds sceneColor on the haze slot; FS
//      branchless composite collapses to `raw * (1 - 0) = raw` —
//      byte-equivalent to pre-S4 renders.
//   2. hazeEnabled == false ⇒ ensureFbo NEVER called (F4: the FG
//      never even created HazeHalf ⇒ 0 alloc). Mirrors
//      frame-graph-mvp.md §7 第 3 条.
//   3. depthHazePass == nullptr (custom desc omits DepthHaze slot) ⇒
//      PostProcessPass haze sampler path binds sceneColor; FS
//      branchless composite collapses to `raw * (1 - 0) = raw`.
//      Mirror §S1c bloomBlurPass==nullptr invariant.
//   4. Deferred: dist = length(GBuffer RT2 worldPos - camPos);
//      Forward / no gbufferMotionRt ⇒ render() central
//      `hazePassEnabled` is false ⇒ FG compile doesn't add
//      HazeHalf ⇒ resolve() returns invalid ⇒ execute returns 0.
//      Avoids the failed D3D invVP reconstruct path.
//   5. ABI: append-only — RenderPassSlot::DepthHaze = 10
//      (unchanged from S4b). View ids: Haze=13, Final PP=14 (Haze
//      before PP so same-frame sampling works). FG does NOT
//      allocate view ids.

#include "AYShader/ShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class DepthHazePass : public RenderPass {
public:
    // §S4 view map lock: BloomExtract=10 → BlurH=11 → BlurV=12 →
    // DepthHaze=13 → PostProcess=14 → UI=255. Haze before Final PP.
    static constexpr uint8_t kDepthHazeViewId = 13;

    DepthHazePass() = default;
    // Mirror BloomExtractPass + PostProcessPass + BloomBlurPass:
    // dtor does NOT touch bgfx handles. BGFXAdapter::shutdown()
    // invalidates globally. For mid-frame teardown, call
    // destroyResources() explicitly first.
    ~DepthHazePass() override = default;

    std::string_view name() const override { return "DepthHaze"; }

    uint32_t execute(PassExecContext& ctx) override;

    // §F4 (2026-07-24) — readiness probe. F4 ships with FG 物理创建
    // 延后到 F6;isReady() 反映 "HazeHalf 是否真 live + valid"。
    // 当前恒 false ── 与 BloomExtractPass F2 + BloomBlurPass F3 守
    // 同样占位。F6 真打开后会从 FG 读。
    bool isReady() const noexcept { return false; }

    // §F4 deprecated (2026-07-24) — halfResFbo() getter used to
    // hand the bloom chain's haze result to PostProcessPass (S4c
    // pattern). After F4 / F5 migration, the consumer pathway goes
    // through `ctx.frameGraph->resolveSemantic(
    // FgSemantic::HazeSource)` (F5); halfResFbo() becomes a legacy
    // shim returning BGFX_INVALID_HANDLE, and F5 removes it.
    //
    // halfWidth()/halfHeight() likewise reflect that this Pass no
    // longer owns a private FBO — F4 ships them as 0 because the
    // FrameGraph owns HazeHalf's physical size. F5 will let them
    // re-read FG (or remove the getter entirely if no test still
    // pins them).
    uint16_t halfWidth()  const noexcept { return 0; }
    uint16_t halfHeight() const noexcept { return 0; }
    bgfx::FrameBufferHandle halfResFbo() const noexcept {
        return BGFX_INVALID_HANDLE;
    }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). §F4 — F4 ships with no FBO to release;
    // destroyResources only releases the Phoskia program + the
    // fullscreen VB/IB. FG-owned HazeHalf is released by
    // FrameGraph::shutdown / FrameGraph::resize (Renderer's Impl
    // shutdown path calls fg.shutdown()).
    void destroyResources(BGFXAdapter& adapter);

private:
    // §F4 (2026-07-24) — `_fbo` / `_fboWidth` / `_fboHeight`
    // migrated out to FrameGraph.HazeHalf. DepthHazePass no longer
    // owns a private FBO. The only per-Pass state that remains is:
    // the fullscreen VB/IB + the Phoskia program. Mirror
    // BloomExtractPass F2 + BloomBlurPass F3 contract.
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;

    // §S4b (2026-07-23) — Phoskia program for the exponential haze
    // effect. Acquired lazily on first execute() after adapter init.
    // Acquire may fail (shaderc missing on CI / disk cache miss +
    // parse error); in that case isReady() stays false and
    // execute() degrades to "early-return 0" (PostProcessPass S4c
    // haze sampler path then binds sceneColor ⇒ byte-equivalent to
    // hazeEnabled=false ⇒ K3 invariant #2 — F4: the resolve() also
    // returns invalid in that case so the early-return path is
    // crossed twice).
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on first acquire; InvalidBinding
    // means "not yet resolved / acquire failed".
    ayt::shader::BindingId      _uHazeDensity     = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uHazeStrength    = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uHazeColor       = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uCamPos          = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSceneColor      = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tWorldPosOrDepth = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every frame
    // (mirror BloomExtractPass / BloomBlurPass).
    bool                        _programAcquireFailed = false;

    // R5+ helpers — only VB/IB + program. ensureFbo removed (F4
    // migration; FG owns HazeHalf now).
    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

// §S4b (2026-07-23) — Bug fix #3 mirror (see BloomExtractPass.h
// for the originating pattern, mirrored by BloomBlurPass + SkyboxPass
// + LightingPass). Externalize the cache-key literal so unit tests
// can include this header and compare their mirror against the live
// literal. Pre-S4b, kDepthHazeCacheKey was a `.cpp` static (not
// addressable from outside), so tests would fall back to string
// self-comparison and the drift detection would be a no-op (false
// green). The extern declaration gives every test a single source
// of truth; drift = test fails immediately.
//
// Naming: `kDepthHazeCacheKeyCStr` (CStr suffix = "raw C-string"
// per the AY naming rules). The actual string literal lives in
// DepthHazePass.cpp as the canonical definition.
extern const char* const kDepthHazeCacheKeyCStr;

} // namespace ayt::render::detail
