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

#include "AYRenderer/RenderScene.h"
#include "AYShader/ShaderResourcePool.h"
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

// GBufferPass owns the deferred MRT: albedo/metallic, normal/roughness,
// world-position/packed material data, emissive/coverage, and depth.
// Consumers borrow it through PassExecContext without exposing GPU handles in
// FrameContext.
class GBufferPass;

// LightingPass produces scene-linear RGBA16F color from the GBuffer,
// scene lights, shadows, sky, and IBL. Production color dependencies resolve
// through the blackboard; the forward declaration keeps metadata fallback
// users from pulling the complete pass definition into every TU.
class LightingPass;

// §Skybox0 (2026-07-23) — SkyboxPass writes the equirect-panorama
// backdrop FBO. Forward decl mirrors the LightingPass pattern so
// PassExecContext can carry the borrowed pointer without dragging
// the full SkyboxPass definition into every TU that already
// includes PassExecContext.h.
class SkyboxPass;

// BloomExtract writes BloomBright and publishes it to the resource blackboard.
// The pass pointer below is retained only for direct-context compatibility.
class BloomExtractPass;

// BloomBlur writes BloomBlurA/B and publishes both to the resource blackboard.
// Its pass pointer is retained only for direct-context compatibility.
class BloomBlurPass;

// SSAO publishes current-frame AO to the resource blackboard. Its pass pointer
// is retained only for direct contexts that intentionally omit the blackboard.
class SSAOPass;

// Deferred RG16F velocity producer consumed by TAA and future temporal passes.
class MotionVectorPass;

// DepthHaze publishes the FrameGraph-owned, full-resolution HazeColor result
// to the resource blackboard. Its pointer is a direct-context fallback.
class DepthHazePass;

// §F2 (2026-07-24, mid-term cutsheet `docs/frame-graph-mvp.md`
// F2 sub-cut) — FrameGraph is the post-process chain resource
// pool (BloomExtract / BloomBlur / DepthHaze / Final). Forward
// declaration mirrors the LightingPass / BloomBlurPass / etc.
// pattern above; PassExecContext carries the borrowed pointer so
// Pass::execute() can resolve a logical resource (FgResourceId)
// to a physical bgfx::FrameBufferHandle.
//
// Lifetime: the FrameGraph is owned by Renderer::Impl (single
// instance, lifetime = Renderer lifetime). PassExecContext holds
// a borrowed, non-owning pointer; safe across dispatch (FG
// outlives any single render() call).
//
// Default-init = nullptr so existing 23-/24-field brace-init
// test sites (Test_F2_ForwardShadow, Test_BloomExtract_S1a, ...,
// Test_FgResource_F1) keep compiling without edits via C++14
// trailing-default behavior. Pre-F2 callers that never wired
// `frameGraph` see early-return 0 in BloomExtract / BloomBlur /
// DepthHaze / PostProcess consume paths (F2-F5 migrate those
// one at a time; F1 leaves everything nullptr-compatible).
class FrameGraph;
struct FrameDrawLists;
class RenderResourceBlackboard;



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
    // shaded color. Production consumers resolve LightingColor through
    // resourceBlackboard; this pointer remains for pass metadata and
    // direct contexts that intentionally omit the blackboard. nullptr
    // means no LightingPass is mounted (for example the Forward path).
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

    // §Skybox0 (2026-07-23) — borrowed, non-owning pointer to the
    // host-supplied Skybox DataSource (ayt::render::SkySource).
    // Drives the SkyboxPass's fullscreen-triangle FS (and
    // indirectly the LightingPass's gbufferSky backdrop sampler).
    // Mirror SceneLights / shadowPass / gbufferPass / lightingPass
    // borrowed pointer pattern; lifetime contract: pointer must
    // remain valid for the duration of pipeline::executeAll(ctx).
    //
    // Why this lives here (not on FrameContext / RenderScene): both
    // are forbidden per cutsheet §5.3 red lines — sky is a
    // *scene-exterior* property (like lighting), not a per-frame
    // camera state and not a per-DrawItem attribute. The host
    // supplies the sky via Renderer::setSkySource() and the
    // renderer reads it via this borrowed pointer.
    //
    // Default-init = nullptr ⇒ SkyboxPass early-returns 0
    // (LightingPass binds no gbufferSky sampler; final image is
    // ambient-only when geometry is missing — byte-equivalent to
    // pre-§Skybox0 behavior on a non-sky host). All existing
    // 18-field brace-init test sites (Test_B7_MultiLightAccumulation
    // ::b7_pass_exec_context_brace_init_default,
    // Test_B5p5_LightingShadow::b5p5_full_pipeline_shadow_borrow_
    // pointer_e2e, ...) keep compiling without edits via C++14
    // trailing-default behavior.
    const ayt::render::SkySource* skySource = nullptr;

    // §Skybox0 (2026-07-23) — borrowed, non-owning pointer to the
    // SkyboxPass that owns the active sky source metadata. Production
    // consumers resolve SkyboxColor through resourceBlackboard; this
    // pointer remains for cube/equirect metadata and direct contexts that
    // intentionally omit the blackboard. It follows the same borrowed
    // lifetime contract as the other pass pointers.
    //
    // Default-init = nullptr ⇒ LightingPass binds no gbufferSky
    // sampler (FS `sample(gbufferSky, ...)` returns black; the
    // `mix(black, lit, coverage)` collapses to `lit` when coverage
    // is high; when coverage is low, the black sky contributes
    // zero — byte-equivalent to pre-§Skybox0 dark-frame behavior
    // on a Forward / non-sky host).
    const SkyboxPass* skyboxPass = nullptr;

    // §P5.5 C (2026-07-23) — borrowed, non-owning pointer to the
    // host-supplied per-light shadow source (cutsheet reservation
    // pass-lessons-from-deferred.md:330 — "wires per-light shadow
    // via PassExecContext::perLightShadows borrowed ptr"). Mirror
    // the `sceneLights` / `skySource` borrowed-pointer pattern;
    // lifetime contract: pointer must remain valid for the duration
    // of pipeline::executeAll(ctx).
    //
    // In practice, this points to the SAME SceneLights instance as
    // `ctx.sceneLights` — the host populates one SceneLights and
    // the renderer reads it through both ptrs (ShadowPass consumes
    // `castShadow` flags + builds per-slot LVP; LightingPass
    // consumes the lights[] array for accumulation and the
    // per-slot shadow uniforms for shadow multiply). Cutsheet
    // reserves the separate field name for future cuts where the
    // host may want to drive ShadowPass with a DIFFERENT
    // per-light shadow source than LightingPass (e.g. fewer
    // shadow casters than accumulation lights).
    //
    // Default-init = nullptr ⇒ ShadowPass falls back to the
    // single-key-light behavior (pre-C byte-equivalent — casts
    // shadow only from FrameContext::lightDirection into one
    // atlas sub-rect[0]); LightingPass uploads
    // perLightShadowCount=0 (FS skips per-light shadow loop —
    // byte-equivalent to pre-C key-only shadow multiply path).
    // All existing 19-field brace-init test sites (Test_B5p5,
    // Test_B7, Test_Skybox0, ...) keep compiling without edits
    // via C++14 trailing-default behavior.
    const ayt::render::SceneLights* perLightShadows = nullptr;

    // Legacy direct-context compatibility. Production Bloom dependencies and
    // current-frame freshness resolve through resourceBlackboard; these
    // borrowed pointers preserve focused tests and custom callers that omit it.
    const BloomExtractPass* bloomExtractPass = nullptr;

    const BloomBlurPass* bloomBlurPass = nullptr;

    // Legacy direct-test compatibility. Production consumers use the resource
    // blackboard's DepthHazeColor entry for current-frame freshness.
    const DepthHazePass* depthHazePass = nullptr;

    // §F2 (2026-07-24, mid-term FG MVP F2) — borrowed, non-owning
    // pointer to the FrameGraph that owns the post-process chain
    // transient resources (BloomBright / BloomBlurA/B / HazeColor / SSAO).
    // BloomExtract (F2) / BloomBlur (F3) / DepthHaze (F4) /
    // PostProcess (F5) read this to resolve a logical FgResourceId
    // to a physical bgfx::FrameBufferHandle. The FrameGraph is
    // owned by Renderer::Impl; its lifetime is the Renderer.
    //
    // Default-init = nullptr preserves the C++14 trailing-default
    // behavior for every existing 22-/23-field brace-init test
    // site. F1 doesn't wire frameGraph anywhere; F2-F5 wire it
    // inside Renderer::render() right before pipeline.executeAll.
    // Pre-F2 callers that never set frameGraph see early-return 0
    // in the consuming Pass paths (the same byte-equivalent
    // behavior as the F2 "host bloomStrength=0" path).
    FrameGraph*          frameGraph     = nullptr;

    // Scene-linear bloom extraction controls. Appended so legacy aggregate
    // initializers retain their previous field mapping.
    float                bloomThreshold = 1.0f;
    float                bloomSoftKnee  = 0.5f;

    // Appended to preserve every existing aggregate initializer's mapping.
    // Production Lighting reads SsaoOcclusion from resourceBlackboard; this
    // pointer remains for direct contexts that intentionally omit it.
    const SSAOPass*      ssaoPass       = nullptr;

    // Tail-appended borrowed producer pointer. MotionVectorPass executes
    // immediately after GBuffer and publishes a current-frame latch; TAA must
    // ignore the persistent texture whenever this pointer/latch is absent.
    const MotionVectorPass* motionVectorPass = nullptr;

    // Per-dispatch Scene View presentation. This must never be implemented
    // through BGFX_DEBUG_WIREFRAME because that flag affects every view,
    // including the editor UI.
    bool wireframe = false;

    // One classification/sort per Renderer::render(). Geometry passes borrow
    // immutable pointers into RenderScene instead of independently scanning
    // and routing every item. nullptr preserves hand-built legacy test paths.
    const FrameDrawLists* drawLists = nullptr;

    // Renderer-owned registry for pass outputs and temporal history. Tail
    // placement preserves legacy aggregate initializers. nullptr keeps direct
    // unit contexts on their compatibility path.
    RenderResourceBlackboard* resourceBlackboard = nullptr;

};

} // namespace ayt::render::detail
