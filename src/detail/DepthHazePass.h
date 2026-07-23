#pragma once

// §S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — DepthHazePass
// real implementation. The S4a skeleton cut reserved the
// RenderPassSlot + PassExecContext borrowed ptr + class shape; S4b
// lands the exponential fog shader (主人拍板 B), the half-resolution
// RGBA8 FBO ensure, the deferred-gbuffer-RT2 / forward-FS-recon
// branch (主人拍板 B — S4b simplification: bind sceneColor on the
// depth slot so the FS compiles; S4d swaps for a proper worldPos
// attachment), and the hazeEnabled/hazeStrength zero-cost gate
// (K3 invariant #2: frame.hazeEnabled=false ⇒ ensureFbo NOT called
// ⇒ no FBO allocation; mirrors frame-graph-mvp.md §7 第 3 条).
//
// Pipeline position (short-term-plan §S4 决策 2026-07-23):
//   ... Lighting → Transparent → BloomExtract → BloomBlur → DepthHaze → PostProcess
// haze only modifies the raw scene color (exponential
// `1 - exp(-density * dist)`); bloom stays independent (per
// 决策 "haze 只改 raw, bloom 独立"). PostProcessPass S4c will sample
// `_fbo` (RT0 of haze result) as the `hazeTexture` sampler on the
// fullscreen composite draw — S4c owns that wire.
//
// View id allocation: S4a locked `kDepthHazeViewId = 14`. S4b draws
// on this view (mirror BloomExtractPass::kBloomExtractViewId).
//
// Phoskia uniform gates (lessons §3.1): all scalars as `uniform vec4`
// with .x carry. hazeColor as vec4 with .xyz used (.w zero pad).
//
// Noop-backend safety: dual guard `!isInitialized() || isNoopBackend()`
// (mirror BloomExtractPass / PostProcessPass / BloomBlurPass /
// ShadowPass / GBufferPass / LightingPass / SkyboxPass). K3 invariant
// #2 + #3 still hold under the real implementation.
//
// K3 invariants (must survive §S4c PostProcessPass haze sampler wire
// + §S4d Editor default-knob polish):
//   1. hazeStrength <= 0 OR hazeEnabled == false ⇒ DepthHazePass
//      early-returns 0 BEFORE ensureFbo; PostProcessPass S4c haze
//      sampler path binds sceneColor on the haze slot; FS branchless
//      composite collapses to `raw * (1 - 0) = raw` — byte-equivalent
//      to pre-S4 renders.
//   2. hazeEnabled == false ⇒ ensureFbo NEVER called (no half-res
//      RT allocation, no view id collision). Mirrors
//      frame-graph-mvp.md §7 第 3 条.
//   3. depthHazePass == nullptr (custom desc omits DepthHaze slot) ⇒
//      PostProcessPass haze sampler path binds sceneColor; FS branchless
//      composite collapses to `raw * (1 - 0) = raw`. Mirror
//      §S1c bloomBlurPass==nullptr invariant.
//   4. dist proxy = luminance of worldPosOrDepth sampler; bounded
//      [0, 1] so `exp(-density * dist)` is well-defined. S4d will
//      swap the second sampler for a proper GBuffer RT2 / FS-recon
//      attachment (proper worldPos-encoded distance).
//   5. ABI: append-only — RenderPassSlot::DepthHaze = 10 (was unused
//      in §S1 cutsheet); view id 14 reserved; no existing enum /
//      view-id value reorders.

#include "AYShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class DepthHazePass : public RenderPass {
public:
    // §S4 view map lock (cutsheet §S1 + §S4 决策): BloomExtract=10 →
    // BlurH=11 → BlurV=12 → PostProcess=13 → DepthHaze=14 → UI=255.
    // S4b draws on view 14.
    static constexpr uint8_t kDepthHazeViewId = 14;

    DepthHazePass() = default;
    // Mirror BloomExtractPass + PostProcessPass + BloomBlurPass:
    // dtor does NOT touch bgfx handles. BGFXAdapter::shutdown()
    // invalidates globally. For mid-frame teardown, call
    // destroyResources() explicitly first.
    ~DepthHazePass() override = default;

    std::string_view name() const override { return "DepthHaze"; }

    uint32_t execute(PassExecContext& ctx) override;

    // R5+ mirror — query whether the pass has a real FBO + program
    // wired. S4b: true after first execute() has built the FBO
    // (gated on hazeEnabled + hazeStrength > 0). False when the
    // host has not opted in OR the Phoskia program couldn't be
    // acquired (shaderc missing on CI). Useful for hosts that want
    // to skip the slot via setEnabled(false) on a stuck state.
    bool isReady() const noexcept { return bgfx::isValid(_fbo); }

    // Host-facing half-resolution size getter (mirror BloomExtractPass).
    // Returns 0 until ensureFbo runs at least once (only happens
    // when hazeEnabled && hazeStrength > 0).
    uint16_t halfWidth()  const noexcept { return _fboWidth;  }
    uint16_t halfHeight() const noexcept { return _fboHeight; }

    // §S4c (2026-07-23) consumer entry point. PostProcessPass will
    // read this FBO (RT0 of the haze result) and bind it as the
    // `hazeTexture` sampler on the fullscreen composite draw,
    // applying `mix(raw, fogColor, fogFactor)` over the un-bloomed
    // raw scene color (haze does NOT touch the bloom chain —
    // mirrors BloomExtractPass::halfResFbo() producer-state pattern).
    // Returns BGFX_INVALID_HANDLE when ensureFbo was skipped
    // (hazeEnabled=false ⇒ no allocation, K3 invariant #2).
    bgfx::FrameBufferHandle halfResFbo() const noexcept { return _fbo; }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). Mirror BloomExtractPass::destroyResources
    // contract. Idempotent (BGFXAdapter::destroy on invalid handle
    // is a no-op).
    void destroyResources(BGFXAdapter& adapter);

private:
    // Lazy FBO — half-resolution RGBA8, no depth. Stays invalid
    // when hazeEnabled=false (K3 invariant #2: execute() short-
    // circuits BEFORE ensureFbo). Mirror BloomExtractPass contract.
    bgfx::FrameBufferHandle    _fbo = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;
    uint16_t                   _fboWidth  = 0;
    uint16_t                   _fboHeight = 0;

    // Phoskia program for the exponential haze effect. Acquired
    // lazily on first execute() after adapter init. Acquire may
    // fail (shaderc missing on CI / disk cache miss + parse
    // error); in that case isReady() stays false and execute()
    // degrades to "early-return 0" (PostProcessPass S4c haze sampler
    // path then binds sceneColor ⇒ byte-equivalent to
    // hazeEnabled=false ⇒ K3 invariant #2).
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on first acquire; InvalidBinding
    // means "not yet resolved / acquire failed".
    ayt::shader::BindingId      _uHazeDensity     = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uHazeStrength    = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uHazeColor       = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSceneColor      = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tWorldPosOrDepth = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every frame
    // (mirror BloomExtractPass / BloomBlurPass).
    bool                        _programAcquireFailed = false;

    // R5+ helpers — gated on hazeEnabled + hazeStrength at the
    // execute() entry. BGFXAdapter gates on isInitialized(), so
    // headless tests run clean.
    void ensureFbo(BGFXAdapter& adapter, uint16_t viewportW, uint16_t viewportH);
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
