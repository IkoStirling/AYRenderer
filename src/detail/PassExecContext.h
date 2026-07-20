#pragma once

// P1 (2026-07-20, PR-C) — collapsed 12-argument RenderPass::execute
// signature. Bundles everything a pass needs to issue one frame's
// draws except for the things that are intrinsic to the pass itself
// (its name, its enable flag, its FBO / program caches).
//
// Why now:
//   - Adding new render-pass machinery (ShadowMap slot, GBuffer MRT,
//     offscreen scene RT for PostProcess closure, future DrawListBuilder
//     state) without this struct means every addition touches ALL
//     pass signatures AND every call site. We already saw the cost in
//     the cut-1 bisect (§5 of docs/execution-plan.md) when an extra
//     FrameContext field implied a 6-TU signature rewrite.
//   - The 12-arg call site in RenderPipeline::executeAll / Renderer::render
//     is fragile — argument swaps at the call site are silent because
//     every parameter is the same POD-ish type (`uint16_t viewportX` vs
//     `uint16_t viewportWidth`). A struct puts each field behind a
//     name.
//
// What stays OUT of the struct (intentional):
//   - The pass's own `name()` and `isEnabled()` state — those belong
//     to the RenderPass subclass, not the per-frame dispatch payload.
//   - Per-pass cached GPU resources (FBO handle, fullscreen VB/IB,
//     program handle). They're allocated once and live on the pass.
//   - Anything that would force `frame` to become non-const. Per
//     docs/execution-plan.md §5.3, the FrameContext reference stays
//     const until the §5.4 isolation experiments prove expanding it
//     is safe. This struct lets us add new fields (e.g. ShadowMap
//     slot, scene RT handle) without making `frame` mutable.
//
// `materials` is the only non-const map reference because Pass
// implementations lazily resolve BindingIds on first use and cache
// them in GpuMaterial::colorBinding / mat4Binding / boneBlockBinding
// (see RenderPass.cpp::resolveAndApplyColorUniforms). Pass-internal
// mutation, not frame-state mutation.

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <unordered_map>

namespace ayt::render::detail
{

// Forward declaration — ShadowPass is the F2 producer of light-space
// matrices + depth FBO. PassExecContext holds a borrowed, non-owning
// pointer so Forward/Transparent can read it without FrameContext
// mutability (per docs/execution-plan.md §5.3 + §5.4 E6).
class ShadowPass;



struct PassExecContext {
    BGFXAdapter&            adapter;
    shader::ShaderResourcePool& pool;
    const RenderScene&      scene;
    const std::unordered_map<uint64_t, GpuMesh>&     meshes;
    const std::unordered_map<uint64_t, GpuTexture>&  textures;
    std::unordered_map<uint64_t, GpuMaterial>&       materials;

    // Viewport sub-rect this pass owns. UI passes typically take
    // the full window rect; scene passes take the 3D viewport.
    uint16_t                viewportX      = 0;
    uint16_t                viewportY      = 0;
    uint16_t                viewportWidth  = 0;
    uint16_t                viewportHeight = 0;

    // Frame data. Const by design (§5.3) — additions are append-only
    // POD fields, not state mutations.
    const FrameContext&     frame;

    // bgfx view id this pass should bind / submit into. Composite
    // mode hands 1 to the scene passes; non-composite hands 0. UIPass
    // ignores it (UIRenderBackend hard-codes kViewId = 2 today).
    uint8_t                 viewId         = 0;

    // P2 (PR-D, 2026-07-20) — shared scene color/depth FBO owned by
    // Renderer::Impl. ForwardOpaquePass + TransparentPass bind it as
    // their view's draw target (instead of the default backbuffer) so
    // PostProcessPass can sample the scene color via attach0.
    //
    // Passes must treat BGFX_INVALID_HANDLE as "no scene RT for this
    // frame → use default backbuffer" (legacy behavior). This protects
    // the headless test path (Noop backend ⇒ Impl never produces a
    // valid sceneFbo) and gives hosts a clean off-switch via
    // `Renderer::setSceneRenderTargetEnabled(false)` once we add it.
    //
    // The FBO is borrowed, not owned, by PassExecContext — Impl owns
    // the underlying handle and rebuilds it on resize(). Passes that
    // want to bind the default backbuffer again (PostProcessPass's
    // post-blit-back submit) call `adapter.setViewFrameBuffer(viewId,
    // BGFX_INVALID_HANDLE)` themselves; this field stays untouched.
    bgfx::FrameBufferHandle sceneFbo       = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};

    // PR-F2 (2026-07-21) — borrowed, non-owning pointer to the
    // ShadowPass that produced the active shadow map this frame.
    // Hosts wire this in pipeline-build / per-frame setup (typically
    // right after dispatching the shadow pass); it is read by
    // Forward/Transparent passes to upload `u_lightViewProj` and
    // bind `shadowMap` (the shadow FBO's depth attachment). nullptr
    // ⇒ forward passes fall back to no-shadow (skip u_lightViewProj
    // upload, skip shadow sampler bind) — matches PR-F1' default of
    // "shadow not in default pipeline".
    //
    // Lifetime: the pointer must remain valid for the duration of
    // pipeline::executeAll(ctx). RenderPipeline outlives every
    // execute() call (the passes are owned via unique_ptr on the
    // pipeline), so a pipeline-resident ShadowPass is safe across
    // dispatch.
    //
    // Why this lives here (not on FrameContext): FrameContext is
    // `const` per §5.3 + writes there cause ABI churn when matrix
    // fields change. A non-owning pointer on the per-dispatch
    // PassExecContext keeps the FrameContext ABI stable (PR-F1'
    // invariant) while still letting forward passes read the
    // shadow producer.
    const ShadowPass*         shadowPass     = nullptr;
};

} // namespace ayt::render::detail