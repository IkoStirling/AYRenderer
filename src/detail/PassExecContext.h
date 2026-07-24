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

// §Skybox0 (2026-07-23) — SkyboxPass writes the equirect-panorama
// backdrop FBO. Forward decl mirrors the LightingPass pattern so
// PassExecContext can carry the borrowed pointer without dragging
// the full SkyboxPass definition into every TU that already
// includes PassExecContext.h.
class SkyboxPass;

// §S1a BloomExtract (2026-07-23) — BloomExtractPass writes the
// half-resolution bright-extract FBO. Forward decl mirrors the
// SkyboxPass pattern so PassExecContext can carry the borrowed
// pointer without dragging the full BloomExtractPass definition
// into every TU that already includes PassExecContext.h. S1a
// itself does NOT use this pointer (it reads upstream scene
// color via PostProcessPass::selectSourceFbo per cutsheet §P5
// B6 lock); S1b BloomBlurPass is the first consumer.
class BloomExtractPass;

// §S1c (2026-07-23, short-term-plan §S1 sub-cut 3) — BloomBlurPass
// produces the half-resolution vertically-blurred FBO that the
// final PostProcess composite samples as the actual bloom
// contribution. Forward decl mirrors BloomExtractPass above;
// PostProcessPass::execute reads `ctx.bloomBlurPass->pongFbo()`
// (RT0 of the blur result) via the borrowed pointer. S1c is the
// first consumer.
class BloomBlurPass;

// §S4a (2026-07-23, short-term-plan §S4 sub-cut 1) — DepthHazePass
// produces the half-resolution depth-aware haze FBO that the final
// PostProcess composite samples as the additional haze contribution
// (mixed over the *un-bloomed* raw scene color, NOT over the
// bloomed composite — per short-term-plan §S4 决策 2026-07-23:
// "haze 只改 raw, bloom 独立"). Forward decl mirrors BloomBlurPass
// above; PostProcessPass::execute (after §S4c) will read
// `ctx.depthHazePass->halfResFbo()` (RT0 of the haze result) via
// the borrowed pointer. §S4a is the SKELETON cut — the type is
// declared, PassExecContext carries the field, but DepthHazePass
// itself currently early-returns 0 from execute() (the real
// shader + FBO ensure land in §S4b).
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
    // SkyboxPass that produced the active sky FBO this frame.
    // LightingPass reads `ctx.skyboxPass->skyRt()` to bind the
    // gbufferSky backdrop sampler. Mirrors the gbufferPass /
    // lightingPass / shadowPass borrowed-pointer pattern; same
    // lifetime contract. Forward-declared at the top of this
    // header (mirror LightingPass forward-decl at the top of
    // PassExecContext.h).
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

    // §S1b BloomBlur (2026-07-23, short-term-plan §S1 sub-cut 2) —
    // borrowed, non-owning pointer to the BloomExtractPass that
    // produced the half-resolution bright FBO this frame. BloomBlurPass
    // reads `ctx.bloomExtractPass->halfResFbo()` (RT0 of the bright
    // extract) as its blur source and ping-pongs into two of its own
    // halfW × halfH FBOs (horizontal pass → vertical pass). Mirrors
    // the skyboxPass / lightingPass / gbufferPass / shadowPass
    // borrowed-pointer pattern; lifetime contract: pointer must
    // remain valid for the duration of pipeline::executeAll(ctx).
    //
    // Default-init = nullptr ⇒ BloomBlurPass early-returns 0 (no
    // source FBO to read; visually identical to bloomStrength=0
    // default — the BloomExtract slot in makeDefault() is optional
    // from the host's perspective; custom desc that omits both
    // BloomExtract and BloomBlur yields a zero-bloom pipeline).
    // All existing 20-field brace-init test sites
    // (Test_BloomExtract_S1a, Test_E4_DefaultShadow, Test_E5_DefaultShadow,
    // Test_Skybox0, ...) keep compiling without edits via C++14
    // trailing-default behavior.
    const BloomExtractPass* bloomExtractPass = nullptr;

    // §S1c (2026-07-23, short-term-plan §S1 sub-cut 3) — borrowed,
    // non-owning pointer to the BloomBlurPass that produced the
    // half-resolution vertically-blurred FBO this frame. PostProcessPass
    // reads `ctx.bloomBlurPass->pongFbo()` (RT0 of the blur result)
    // and binds it as an additional sampler on the fullscreen-triangle
    // composite draw, replacing the pre-S1 fake
    // `raw + raw*bloomStrength` shader hack with the real
    // `raw + sample(bloomTexture, uv) * bloomStrength` composite.
    //
    // Mirrors the bloomExtractPass / skyboxPass / lightingPass /
    // gbufferPass / shadowPass borrowed-pointer pattern; same
    // lifetime contract: pointer must remain valid for the duration
    // of pipeline::executeAll(ctx).
    //
    // Default-init = nullptr ⇒ PostProcessPass falls back to the
    // pre-S1 fake composite (`raw + raw*bloomStrength`) which is
    // visually a no-op when bloomStrength=0 (host default). Custom
    // desc that omits both BloomExtract and BloomBlur yields a
    // pipeline where ctx.bloomBlurPass is null AND the composite
    // contribution is `raw * (1 + 0) = raw` (zero bloom) — same
    // visual result as a pipeline without any bloom passes mounted.
    // All existing 20-/21-field brace-init test sites
    // (Test_BloomExtract_S1a, Test_BloomBlur_S1b, Test_PostProcess_R51,
    // Test_E4_DefaultShadow, Test_E5_DefaultShadow, Test_Skybox0, ...)
    // keep compiling without edits via C++14 trailing-default behavior.
    const BloomBlurPass* bloomBlurPass = nullptr;

    // §S4a (2026-07-23, short-term-plan §S4 sub-cut 1) — borrowed,
    // non-owning pointer to the DepthHazePass that produces the
    // half-resolution depth-aware haze FBO. PostProcessPass (after
    // §S4c) reads `ctx.depthHazePass->halfResFbo()` (RT0 of the haze
    // result) and binds it as an additional sampler on the fullscreen
    // composite draw. Mirrors the bloomExtractPass / bloomBlurPass /
    // skyboxPass / lightingPass / gbufferPass / shadowPass
    // borrowed-pointer pattern; same lifetime contract: pointer must
    // remain valid for the duration of pipeline::executeAll(ctx).
    //
    // Default-init = nullptr ⇒ S4c PostProcessPass haze-sampler code
    // path binds `sceneColor` to the haze slot (byte-equivalent to
    // hazeStrength=0 ⇒ no fog applied). Custom desc that omits
    // DepthHaze from the pipeline yields a pipeline with
    // ctx.depthHazePass null AND zero haze contribution — visually
    // identical to a pipeline that never mounted haze (K3 invariant
    // mirrored from S1c).
    //
    // §S4a is the SKELETON cut: only the field declaration + the
    // empty DepthHazePass class land here. Real shader, real FBO
    // ensure, and the PostProcessPass consumer-side wiring all
    // arrive in §S4b / §S4c.
    //
    // All existing 20-/21-/22-field brace-init test sites keep
    // compiling without edits via C++14 trailing-default behavior.
    const DepthHazePass* depthHazePass = nullptr;

    // §F2 (2026-07-24, mid-term FG MVP F2) — borrowed, non-owning
    // pointer to the FrameGraph that owns the post-process chain
    // transient resources (BloomBright / BloomBlurA/B / HazeHalf).
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

    // V1 GBuffer Debug (2026-07-24) — Renderer::Impl-owned
    // offscreen RT (view 250). Mirrors sceneFbo (line 180):
    // host-owned, borrowed, NOT in FG. Default INVALID ⇒
    // GBufferDebugPass early-returns 0 (zero draw, zero alloc).
    // Set in render() ctx brace-init only when gbufferDebugEnabled
    // is true AND the FBO lazy-ensure succeeded. Append-only
    // trailing default ⇒ all existing 23-field brace-init test
    // sites keep compiling without edits.
    bgfx::FrameBufferHandle gbufferDebugFbo =
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
};

} // namespace ayt::render::detail