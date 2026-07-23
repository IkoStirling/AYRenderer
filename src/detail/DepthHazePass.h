#pragma once

// §S4a (2026-07-23, short-term-plan §S4 sub-cut 1) — DepthHazePass
// skeleton cut. Mirrors BloomExtractPass.h interface shape
// (RenderPass-derived, lazy FBO ensure, borrowed-ptr consumer via
// halfResFbo() getter, IsNoopBackend safe, append-only view id 14)
// but execute() currently early-returns 0. The real shader (exponential
// haze + Deferred worldPos OR Forward FS reconstruction), the
// FBO-ensure gate on enabled, and the PostProcessPass consumer-side
// wiring all land in §S4b / §S4c.
//
// Why now: §S3 短期收口决策 (2026-07-23) "先做 §S4 再开 FG"。§S4
// 把 frame-graph-mvp.md §7 第 3 条 "关效果即不分配 RT" 做出来:
// DepthHazePass::execute() 在 haze enabled=false 时 **不调**
// ensureFbo()，与今日 (no DepthHazePass mounted at all) 字节一致。
//
// What S4a ships:
//   - class DepthHazePass declaration (mirror BloomExtractPass.h)
//   - PassExecContext::depthHazePass borrowed ptr (default null)
//   - RenderPassSlot::DepthHaze = 10 (append-only ABI)
//   - execute() body: 0-draw no-op + log line
// What S4a DOES NOT ship (cuts to S4b/S4c):
//   - Real Phoskia haze shader
//   - ensureFbo() actual RGBA8 FBO creation (skeleton method decl only)
//   - Forward-path FS worldPos reconstruction
//   - PostProcessPass hazeTexture sampler wiring
//
// View id allocation: ... Trans-deferred=9 → BloomExtract=10 →
// BlurH=11 → BlurV=12 → PostProcess=13 (existing) → DepthHaze=14.
// 14 is the first contiguous unused id in the §S1 view table.
//
// Lifecycle: half-resolution RGBA8 FBO, no depth, lazy ensure on
// first execute() when hazeEnabled=true (S4b behavior — S4a has
// no FBO at all). Mirror PostProcessPass::ensureFbo contract.
//
// Phoskia uniform gates (lessons §3.1): scalars uploaded as
// `uniform vec4` with .x carry (S4b).
//
// Noop-backend safety: mirror BloomExtractPass dual guard
// `!isInitialized() || isNoopBackend()` → execute() returns 0
// + no side effects. The skeleton cut's execute() unconditionally
// returns 0 anyway, but the guard is documented for S4b to keep.
//
// K3 invariants (must survive S4b/S4c additions):
//   1. hazeStrength == 0 (host default) ⇒ PostProcessPass haze
//      sampler (after §S4c) collapses to sceneColor ⇒ pre-S4
//      zero-behavior-change. Mirror §S1c K3 invariant.
//   2. hazeEnabled=false ⇒ DepthHazePass::execute() does NOT call
//      ensureFbo() ⇒ no FBO allocation ⇒ no view id collision.
//      Mirrors frame-graph-mvp §7 第 3 条.
//   3. depthHazePass==nullptr (custom desc omits DepthHaze slot) ⇒
//      PostProcessPass haze sampler path binds sceneColor (mirror
//      bloomBlurPass=nullptr collapse in §S1c).
//   4. half-res size = (viewportW+1)/2 × (viewportH+1)/2 — same
//      rounding as BloomExtractPass convention.
//   5. ABI: append-only — RenderPassSlot::DepthHaze = 10 (was unused
//      in §S1 cutsheet); no existing enum value reorders.

#include "AYShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class DepthHazePass : public RenderPass {
public:
    // Composite view map (§S4 order lock):
    //   ... BlurH=11 → BlurV=12 → PostProcess=13 → **DepthHaze=14**
    //   → UI=255 (fixed high slot). View 14 is the first contiguous
    //   unused id after §S1b's 12/13 lock. S4b lands the actual
    //   draw; S4a only reserves the id (kept here for symmetry
    //   with BloomExtractPass::kBloomExtractViewId).
    static constexpr uint8_t kDepthHazeViewId = 14;

    DepthHazePass() = default;
    // Mirror BloomExtractPass + PostProcessPass: dtor does NOT touch
    // bgfx handles. BGFXAdapter::shutdown() invalidates globally.
    // For mid-frame teardown, call destroyResources() explicitly first.
    ~DepthHazePass() override = default;

    std::string_view name() const override { return "DepthHaze"; }

    uint32_t execute(PassExecContext& ctx) override;

    // R5+ mirror — query whether the pass has a real FBO + program
    // wired. S4a: always false (no FBO ensured yet). S4b: true
    // after first execute() has built the FBO. Useful for hosts
    // that want to skip the slot via setEnabled(false) when the
    // Phoskia program cannot be acquired.
    bool isReady() const noexcept { return bgfx::isValid(_fbo); }

    // Host-facing half-resolution size getter (mirror BloomExtractPass).
    // Returns 0 until S4b's ensureFbo runs.
    uint16_t halfWidth()  const noexcept { return _fboWidth;  }
    uint16_t halfHeight() const noexcept { return _fboHeight; }

    // §S4c (2026-07-23) — consumer entry point. PostProcessPass will
    // read this FBO (RT0 of the haze result) and bind it as the
    // `hazeTexture` sampler on the fullscreen composite draw,
    // applying `mix(raw, fogColor, fogFactor)` over the un-bloomed
    // raw scene color (haze does NOT touch the bloom chain —
    // mirrors BloomExtractPass::halfResFbo() producer-state pattern).
    // S4a: returns BGFX_INVALID_HANDLE always (no FBO yet).
    // S4b: returns BGFX_INVALID_HANDLE when ensureFbo was skipped
    // (hazeEnabled=false ⇒ no allocation, K3 invariant #2).
    bgfx::FrameBufferHandle halfResFbo() const noexcept { return _fbo; }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). Mirror BloomExtractPass::destroyResources
    // contract. Idempotent (BGFXAdapter::destroy on invalid handle
    // is a no-op). S4a: no-op (no resources to release). S4b: full
    // FBO + program release.
    void destroyResources(BGFXAdapter& adapter);

private:
    // Lazy FBO — half-resolution RGBA8, no depth. S4a: stays invalid
    // (no FBO created yet — K3 invariant #2 keeps ensure() gated on
    // hazeEnabled=true; S4a's execute() always returns 0 so ensure
    // is never reached). S4b: ensureFbo allocates + resizes on
    // viewport change.
    bgfx::FrameBufferHandle    _fbo = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;
    uint16_t                   _fboWidth  = 0;
    uint16_t                   _fboHeight = 0;

    // Phoskia program for the haze effect. S4a: stays invalid (no
    // shader yet). S4b: acquired lazily on first execute() after
    // adapter init. Acquire may fail (shaderc missing on CI / disk
    // cache miss + parse error); in that case isReady() stays false
    // and execute() degrades to "return 0" (PostProcessPass haze
    // sampler path then binds sceneColor ⇒ byte-equivalent to
    // hazeEnabled=false ⇒ K3 invariant #2).
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. S4a: stay at InvalidBinding (no program).
    // S4b: resolved on first acquire; declared here so S4b only has
    // to fill in the names without touching the class shape.
    ayt::shader::BindingId      _uHazeDensity   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uHazeColor     = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uHazeStrength  = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSceneColor    = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tWorldPosOrDepth = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every frame
    // (mirror BloomExtractPass). S4a: irrelevant (no acquire). S4b:
    // latches after first failed acquire.
    bool                        _programAcquireFailed = false;

    // R5+ helpers. S4a: declarations only, no body. S4b: fills in
    // the haze shader, the FBO allocation, the worldPos vs FS-recon
    // branching, and the hazeEnabled gate.
    void ensureFbo(BGFXAdapter& adapter, uint16_t viewportW, uint16_t viewportH);
    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

} // namespace ayt::render::detail