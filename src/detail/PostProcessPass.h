#pragma once

#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYShader/ShaderResource.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace ayt::render::detail
{

// R5.1 (2026-07-20, commit 9dab8cc) — fullscreen-triangle post-process
// pass. Wires a Phoskia program + u_bloomStrength / u_exposure /
// u_tonemapMode uniforms + sampler bind of `u_sceneColor` + a real
// blit-back of the offscreen FBO to the default backbuffer for UIPass
// to composite chrome over.
//
// Pipeline slot: index 2 of the 4-pass default pipeline (between
// Transparent and UI). See `docs/execution-plan.md` §1.1 / §附录 B.
//
// KNOWN LIMIT (post-R5.1, pre-P2 → closed in PR-D): `u_sceneColor`
// used to sample the pass's OWN empty FBO, not ForwardOpaquePass +
// TransparentPass's actual scene output. The fullscreen triangle
// ran and the blit-back ran, so headless tests passed and the wire
// was end-to-end; visible result was wrong on real GPU backends.
// P2 (PR-D, 2026-07-20) closed the loop: PostProcessPass now prefers
// `ctx.sceneFbo` (the Renderer-owned color+depth FBO ForwardOpaque +
// Transparent drew into) over its own self-FBO. The fallback to
// self-FBO is preserved for the Noop test path + hosts that opt
// into the backbuffer pipeline. UIPass (view 2) and its
// UIRenderBackend are untouched. See docs/execution-plan.md P2 /
// `docs/execution-plan.md` §附录 A for the test pin that this wire
// works without regressions.
//
// §P5 B6 (2026-07-22) — Deferred-path closure. When the active
// pipeline is Deferred (GBuffer + Lighting mount a LightingOutput
// FBO that holds the LIT scene color), PostProcessPass MUST sample
// `ctx.gbufferPass->lightingOutputFbo()` (= the B5 LightingPass
// output RT) instead of `ctx.sceneFbo` (= Renderer-owned FO+Trans
// shared RT, which on Deferred path is empty because FO is skipped
// at cutsheet §5.3 red line). Without this priority flip,
// PostProcessPass on a Deferred pipeline would blit an empty
// sceneFbo — visible blackness on Editor Play.
//
// DepthHaze now produces the full-resolution scene-linear HazeColor result.
// PostProcess selects that target as its primary scene source only after the
// producer's current-frame latch is set; it no longer samples or recomposites
// a separate half-resolution haze texture. SSAO is already part of deferred
// Lighting's ambient term and is never applied to final scene color here.
//
// Source-FBO priority order (B6 lock):
//   1. `ctx.gbufferPass != nullptr && ctx.lightingPass != nullptr
//       && bgfx::isValid(ctx.lightingPass->lightingOutputFbo())`
//       → use `ctx.gbufferPass->lightingOutputFbo()`
//         (deferred path; cutsheet pass-lessons-from-deferred.md:169)
//   2. `BGFXAdapter::isValid(ctx.sceneFbo)`
//       → use `ctx.sceneFbo` (P2 default; forward path + hosts
//         that opt into non-deferred post-process).
//   3. neither valid → return 0 (no-op; matches existing P2 shape).
//
// Algorithm today (2026-07-23): sample sceneColor → exposure/bloom
  // gain → branchless tonemap (None/Reinhard/ACES) → display gamma
  // encode (pow 1/gamma). All scalar knobs are
  // Phoskia `vec4` (.x) for bgfx Vec4 upload ABI. See docs/post-process.md.
  //
  // Historical note (R5.1 / P2 / B6):
  //   1) Acquire the offscreen FBO from BGFXAdapter (create-once,
  //      resize-on-viewport-change tracked here).
  //   2) Source-FBO priority: LightingOutputFbo (deferred, B6) >
  //      sceneFbo (forward, P2). Same-`sceneColor` Phoskia sampler;
  //      no shader changes.
  //   3) Submit fullscreen triangle sampling "sceneColor".
  //   4) Bind default backbuffer; UIPass composites chrome after.
  //   5) Return the draw-call count.
//
// Noop-backend safety: when BGFXAdapter is uninitialized or the FBO
// create fails, the entire execute() body short-circuits to 0 draws
// and 0 side effects. Headless tests in CI rely on this — every
// `createFrameBuffer` and `setViewFrameBuffer` is gated by an
// `_adapter.isInitialized()` check inside BGFXAdapter itself.
//
// Lazy FBO lifecycle: FBO is created on the first execute() call
// after the adapter is initialized, and resized whenever the
// viewport size changes. BGFXAdapter owns the underlying handle;
// this class owns the resource cache and release semantics
// (destroy on shutdown / resize / adapter-reinit).
class PostProcessPass : public RenderPass {
public:
    // Stable final-composite view. Deferred ordering is SSAO(14) →
    // Lighting(8) → DepthHaze(13) → transparent/bloom → PostProcess(15),
    // with UI chrome fixed at view 255.
    static constexpr uint8_t kBlitViewId = 15;

    PostProcessPass() = default;
    // R5+ — destructor intentionally does NOT touch bgfx handles.
    // RenderPass base has no BGFXAdapter reference (passes are
    // adapter-agnostic); passing the adapter in via a method would
    // break the U0 polymorphism contract. bgfx::shutdown() in
    // BGFXAdapter::shutdown() invalidates all handles globally, so
    // releasing at that point is implicit. For mid-frame adapter
    // teardown (rare), call destroyResources() explicitly with the
    // adapter before the pass is destroyed.
    ~PostProcessPass() override = default;

    std::string_view name() const override { return "PostProcess"; }

    uint32_t execute(PassExecContext& ctx) override;

    // §P5 B6 (2026-07-22) — source-FBO priority helper. Single
    // point of truth for the B6 cutsheet closure
    // (`docs/pass-lessons-from-deferred.md:169`):
    //
    //   1. deferred-path LIT color via ctx.gbufferPass->lightingOutputFbo()
    //   2. forward-path shared scene color via ctx.sceneFbo (P2)
    //   3. invalid → caller early-returns 0 (no-op)
    //
    // Static on PassExecContext (not member on PostProcessPass) so
    // tests can pin the priority pin without spinning up
    // PostProcessPass state.
    //
    // No new branch outside this helper: execute() collapses to a
    // single `sourceFbo = selectSourceFbo(ctx)` then an early-out
    // on `!BGFXAdapter::isValid(sourceFbo)`.
    static bgfx::FrameBufferHandle selectSourceFbo(const PassExecContext& ctx) noexcept;

    // R5+ — query whether the pass has a real FBO + program wired.
    // Useful for hosts that want to skip the slot via setEnabled(false)
    // when the post-process pipeline cannot be created (e.g. backend
    // was initialized but the post-process Phoskia program is not in
    // the pool). Today the pass is "ready" once execute() has built
    // the FBO at least once.
    bool isReady() const noexcept { return bgfx::isValid(_fbo); }

private:
    // R5+ — cached FBO. Invalid until first execute(). BGFXAdapter
    // owns the underlying bgfx handle; this class owns the cache +
    // resize/destroy decision (mirrors how RenderResourceManager
    // caches meshes / materials).
    bgfx::FrameBufferHandle    _fbo = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;
    uint16_t                   _fboWidth  = 0;
    uint16_t                   _fboHeight = 0;

    // R5.1 (2026-07-20) — Phoskia program for the post-process effect.
    // Acquired lazily from the shader pool on first execute() after
    // adapter initialization; bound to the fullscreen-triangle draw.
    // Pool acquire may fail (shaderc missing on CI / disk cache miss
    // + parse error); in that case isReady() returns false and
    // execute() degrades to the R5+ "draw geometry only" path (no
    // real blit) — the scene color still appears on screen because
    // ForwardOpaque + Transparent wrote to the default backbuffer
    // before us. Cached for the pass's lifetime (program is hot-reload
    // aware via pool.acquire's cache key).
    ayt::shader::ShaderResource _program;

    // R5.1 — cached uniform / texture binding IDs. Resolved from
    // _program on first acquire (cheaper than getUniformBinding every
    // frame); InvalidBinding (0) means "not yet resolved". Tests use
    // these to pin that the wire path actually found the names.
    ayt::shader::BindingId      _uBloomStrength = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uExposure      = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uTonemapMode   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uTime          = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uGammaParams   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSceneColor    = ayt::shader::InvalidBinding;
    // §S1c (2026-07-23, short-term-plan §S1 sub-cut 3) — second
    // sampler on the fullscreen-triangle composite draw, bound to
    // `ctx.bloomBlurPass->pongFbo()` RT0 when present. Replaces
    // the pre-S1 fake `raw + raw*bloomStrength` shader hack with
    // `raw + sample(bloomTexture, uv) * bloomStrength`. Invalid
    // when the program hasn't been acquired yet (mirror _tSceneColor).
    ayt::shader::BindingId      _tBloomTexture  = ayt::shader::InvalidBinding;
    // Latch so a failed acquire does not re-run shaderc every frame
    // (was the main stutter source when Phoskia→HLSL rejected).
    bool                        _programAcquireFailed = false;

    // R5+ — helpers. Both are no-ops on the Noop backend (BGFXAdapter
    // gates on isInitialized()), so the headless test path runs clean.
    void ensureFbo(BGFXAdapter& adapter, uint16_t width, uint16_t height);
    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
    void destroyResources(BGFXAdapter& adapter);
};

// §S4c (2026-07-23) — Bug fix #3 mirror (see DepthHazePass.h:154-167
// for the most-recent previous application, mirrored by
// BloomExtractPass.h, BloomBlurPass.h:185-199, LightingPass.h).
// Externalize the cache-key literal so unit tests can include this
// header and compare their mirror against the live literal. Pre-S4c,
// kPostProcessCacheKey was a `.cpp` static (not addressable from
// outside), so tests would fall back to string self-comparison
// ("mine == mine") and the drift detection would be a no-op (false
// green). The extern declaration gives every test a single source
// of truth; drift = test fails immediately.
//
// Naming: `kPostProcessCacheKeyCStr` (CStr suffix = "raw C-string"
// per the AY naming rules). The actual string literal lives in
// PostProcessPass.cpp as the canonical definition.
extern const char* const kPostProcessCacheKeyCStr;

// Exact runtime shader sources used by PostProcessPass. Exposed from the
// private detail header so regression tests compile the production strings
// instead of stale test-local mirrors.
const char* postProcessPhoskiaSourceForTests() noexcept;
const char* postProcessFallbackPhoskiaSourceForTests() noexcept;

// §P5 L3 (2026-08-24) — color-format constant extern. Pre-L3 the
// color format was an inline `bgfx::TextureFormat::RGBA8` literal
// inside ensureFbo(); a future sRGB post-process pass flipping the
// format would have to grep the .cpp. Now the enum value is named
// (kPostProcessColorFormat) and exported via this extern so tests
// can pin the choice against the live enum value.
extern const bgfx::TextureFormat::Enum kPostProcessColorFormat;

} // namespace ayt::render::detail
