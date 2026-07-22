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

// §P5 B2 (2026-07-22) — GBufferPass is the future B4 producer of the
// GBuffer MRT (RT0 albedo + RT1 normal + RT2 motion + depth). The
// B5 LightingPass consumes these attachments as its scene-color /
// scene-normal / scene-motion inputs. Until then the shell class
// is empty; this forward decl lets us carry the borrowed pointer on
// PassExecContext in B2 without dragging the full GBufferPass
// definition into every TU that already includes PassExecContext.h.
class GBufferPass;

// §P5 B3 (2026-07-22) — LightingPass is the B5 producer of the
// shaded scene color (1 RGBA8 FBO, fullscreen-triangle FS that
// samples the GBuffer MRT + shadow). Forward decl mirrors the
// GBufferPass pattern so PassExecContext can carry the borrowed
// pointer without dragging the full LightingPass definition into
// every TU that already includes PassExecContext.h.
class LightingPass;



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

    // bgfx view id for scene passes (FO / Transparent). Composite
    // mode hands 3; non-composite hands 0. ShadowPass uses 1 (caster)
    // + 2 (resolve blit); PostProcessPass / UIPass use 4 / 5 — bgfx
    // keeps one FBO+VP per view for the whole frame, so they must not share.
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

    // §P5 B2 (2026-07-22) — borrowed, non-owning pointer to the
    // GBufferPass that fills the GBuffer MRT this frame. Mirrors
    // the shadowPass pattern above: Forward / Deferred-light passes
    // (B5 LightingPass, future B7+ multi-light consumers) read the
    // gbufferFbo / RT0..RT2 / depth attachments through this
    // pointer instead of writing them through FrameContext (§5.3
    // red line: no FrameContext ABI churn).
    //
    // Lifetime: pointer must remain valid for the duration of
    // pipeline::executeAll(ctx). The GBufferPass is owned by the
    // pipeline via unique_ptr, so a pipeline-resident GBufferPass
    // is safe across dispatch (same guarantee as ShadowPass).
    //
    // Default-init = nullptr so existing 12-field brace-init
    // test sites (Test_F2_ForwardShadow, Test_E5_DefaultShadow,
    // Test_ShadowPass, Test_F3_SkinnedCaster, Test_PassExecContext_P1,
    // Test_SceneRT_P2, Test_PostProcess_R5Plus, ...) keep compiling
    // without edits — the new field defaults at the struct's
    // brace-init trailing default (C++14+ behavior, same as
    // PR-F2's shadowPass).
    const GBufferPass*        gbufferPass    = nullptr;

    // §P5 B3 (2026-07-22) — borrowed, non-owning pointer to the
    // LightingPass that consumes the GBuffer MRT + produces scene-
    // shaded color. Mirrors gbufferPass pattern. B5's LightingPass
    // itself doesn't read this (LightingPass *is* the dispatch
    // endpoint); future B7+ multi-light consumers (e.g. a second
    // lighting pass for second bounce, or post-Lighting tone mapping)
    // can read the producer pointer here. nullptr ⇒ no LightingPass
    // mounted (e.g. host on Forward path) — same shape as shadowPass
    // / gbufferPass.
    //
    // Lifetime: pointer must remain valid for the duration of
    // pipeline::executeAll(ctx). The LightingPass is owned by the
    // pipeline via unique_ptr, so a pipeline-resident LightingPass
    // is safe across dispatch (same guarantee as ShadowPass +
    // GBufferPass).
    //
    // Default-init = nullptr so existing 12-/15-field brace-init
    // test sites (Test_F2_ForwardShadow, Test_B2_GBufferPass,
    // Test_E5_DefaultShadow, Test_ShadowPass, Test_F3_SkinnedCaster,
    // Test_PassExecContext_P1, Test_SceneRT_P2, Test_PostProcess_R5Plus,
    // Test_B1_RenderPath, ...) keep compiling without edits — the
    // new field defaults at the struct's brace-init trailing default
    // (C++14+ behavior, same as PR-F2's shadowPass + PR-B2's
    // gbufferPass).
    const LightingPass*       lightingPass   = nullptr;

    // §P5 B7+ (2026-07-22) — borrowed, non-owning pointer to the
    // host-supplied multi-light DataSource (ayt::render::SceneLights).
    // Drives the LightingPass's accumulation loop (B7 multi-light
    // cut). Mirror shadowPass / gbufferPass / lightingPass borrowed
    // pointer pattern; lifetime contract: pointer must remain valid
    // for the duration of pipeline::executeAll(ctx).
    //
    // Why this lives here (not on FrameContext / RenderScene): both
    // are forbidden per cutsheet §5.3 red lines #1 (RenderScene::Light
    // permanently retired per §5.5 cleanup) and #2 (FrameContext must
    // not grow light data fields). Per-frame per-light data sits on
    // the *host* as SceneLights { DirectionalLight[kMaxSceneLights];
    // count; }, and the renderer reads via this borrowed pointer.
    //
    // Default-init = nullptr ⇒ B5 single-light path still works
    // (LightingPass falls back to FrameContext::lightDirection /
    // lightColor when sceneLights is null). All existing 16-field
    // brace-init test sites (Test_B5_LightingDirectional,
    // Test_B6_PostProcessSourceFbo, ...) keep compiling without edits
    // via C++14 trailing-default behavior.
    const ayt::render::SceneLights* sceneLights = nullptr;
};

} // namespace ayt::render::detail