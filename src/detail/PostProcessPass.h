#pragma once

#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYShaderResource.h"

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
// Algorithm today (R5+ minimal — bloom/exposure/tonemap all wired but
// the bundled Phoskia program is a near-identity "passthrough with
// bloom strength + exposure + tonemap knob" composite; future
// iterations can swap the program handle for a full downsample/upsample
// bloom pair):
//   1) Acquire the offscreen FBO from BGFXAdapter (create-once,
//      resize-on-viewport-change tracked here).
//   2) bind FBO as the view's draw target for the post-process viewId.
//   3) Submit a fullscreen triangle (3 verts, 1 instance) reading the
//      scene color from a sampler named "u_sceneColor" (the fragment
//      shader is expected to declare `uniform sampler2D u_sceneColor`).
//      The bundle program reads `u_time`, `u_bloomStrength`,
//      `u_exposure`, `u_tonemapMode` for effect shaping.
//   4) Bind the default backbuffer, blit FBO -> backbuffer.
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
    ayt::shader::BindingId      _tSceneColor    = ayt::shader::InvalidBinding;

    // R5+ — helpers. Both are no-ops on the Noop backend (BGFXAdapter
    // gates on isInitialized()), so the headless test path runs clean.
    void ensureFbo(BGFXAdapter& adapter, uint16_t width, uint16_t height);
    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
    void destroyResources(BGFXAdapter& adapter);
};

} // namespace ayt::render::detail