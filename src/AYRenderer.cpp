#include "AYRenderer.h"

#include "AYF1DiagFlags.h"
#include "detail/BGFXAdapter.h"
#include "detail/BgfxMatrix.h"
#include "detail/BloomExtractPass.h"
#include "detail/BloomBlurPass.h"
#include "detail/DepthHazePass.h"  // S4b (2026-07-23) — borrowed-ptr source for PassExecContext::depthHazePass + destroyResources.
#include "detail/DebugOverlay.h"
#include "detail/FgResource.h"        // §F2 (2026-07-24) — FrameGraph FgResourceId + FgTextureDesc
#include "detail/ForwardOpaquePass.h"
#include "detail/GBufferPass.h"
#include "detail/FrameContext.h"
#include "detail/LightingPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPipeline.h"
#include "detail/RenderResourceManager.h"
#include "detail/ScreenshotSidecar.h"
#include "detail/ShaderPoolSetup.h"
#include "detail/ShadowPass.h"
#include "detail/SkyboxPass.h"
#include "detail/TransparentPass.h"
#include "detail/UiGpuContext.h"
#include "detail/UIPass.h"
#include "AYUIRenderBackend.h"

#include "AYShaderResourcePool.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <algorithm>

namespace ayt::render
{

std::size_t detailDiagSizeofFrameContext()
{
    return sizeof(detail::FrameContext);
}

RenderPipelineDesc RenderPipelineDesc::makeDefault()
{
    // E5 (§5.4, 2026-07-22): default pipeline now mounts Shadow at
    // slot 0 *enabled* (RenderPass base default _enabled == true).
    // Hosts that want shadows get them out of the box; hosts that
    // want to opt out pass a custom desc that omits the Shadow slot.
    // E4's "canonical default ⇒ Shadow disabled" override is removed
    // — its std::equal detection was a no-op distinction anyway
    // (makeDefault() and makeForwardWithShadows() were byte-identical)
    // and the resulting behavior contradicted the test comments.
    return RenderPipelineDesc{{
        RenderPassSlot::Shadow,
        RenderPassSlot::ForwardOpaque,
        RenderPassSlot::Transparent,
        RenderPassSlot::BloomExtract,   // S1a (2026-07-23) — half-res bright extract; bloomStrength=0 default ⇒ zero write.
        RenderPassSlot::BloomBlur,      // S1b (2026-07-23) — half-res separable-Gaussian blur ping-pong; bloomStrength=0 default ⇒ zero write.
        RenderPassSlot::DepthHaze,      // S4b (2026-07-23) — half-res exponential depth-aware haze; hazeEnabled=false default ⇒ zero FBO + zero write (K3 invariant #2).
        RenderPassSlot::PostProcess,
        RenderPassSlot::UI,
    }};
}

RenderPipelineDesc RenderPipelineDesc::makeForwardWithShadows()
{
    // E5: alias for makeDefault() — both expose Shadow enabled. Kept
    // for source compatibility with hosts / Editor that explicitly
    // assemble the shadow-forward pipeline (see
    // AYEditorPlayRuntime.cpp:applyEditorRenderPipeline).
    return makeDefault();
}

RenderPipelineDesc RenderPipelineDesc::makeDeferred()
{
    // §P5 B3 (2026-07-22) — actual Deferred pipeline. 6 slots
    // (Shadow + GBuffer + Lighting + Transparent + PostProcess + UI).
    // ForwardOpaque OMITTED from this list per cutsheet §4.1 red line
    // #4 (no FO re-render after Lighting — Lighting owns the
    // opaque-pass equivalent in Deferred path). Shadow + Trans +
    // PostProcess + UI are shared with Forward. Path = Deferred
    // tag is for hosts / observers that want to query the path.
    // B4 wires real GBuffer MRT on view 7; B5 wires LightingPass
    // fullscreen triangle on view 8.
    //
    // §Skybox0 (2026-07-23) — extended to 7 slots. Skybox added
    // at slot 1, between Shadow and GBuffer. SkyboxPass writes an
    // independent RGBA8 FBO (skyFbo) that LightingPass samples as
    // a backdrop via `texture2d gbufferSky`. The Skybox slot is
    // OPT-IN — `makeDefault()` (Forward) does NOT include it, so
    // Forward hosts that call `setSkySource()` see 0 behavior
    // change (cutsheet §5.3 red line #4). View-id allocation
    // per cutsheet §5.1: SkyboxPass claims the reserved view 6.
    return RenderPipelineDesc{{
        RenderPassSlot::Shadow,
        RenderPassSlot::Skybox,         // §Skybox0 (2026-07-23)
        RenderPassSlot::GBuffer,
        RenderPassSlot::Lighting,
        RenderPassSlot::Transparent,
        RenderPassSlot::BloomExtract,   // S1a (2026-07-23) — half-res bright extract; bloomStrength=0 default ⇒ zero write.
        RenderPassSlot::BloomBlur,      // S1b (2026-07-23) — half-res separable-Gaussian blur ping-pong; bloomStrength=0 default ⇒ zero write.
        RenderPassSlot::DepthHaze,      // S4b (2026-07-23) — half-res exponential depth-aware haze; hazeEnabled=false default ⇒ zero FBO + zero write.
        RenderPassSlot::PostProcess,
        RenderPassSlot::UI,
    }, RenderPath::Deferred};
}

bool RenderPipelineDesc::contains(RenderPassSlot slot) const noexcept
{
    for (const RenderPassSlot s : passes) {
        if (s == slot) {
            return true;
        }
    }
    return false;
}

namespace {

std::unique_ptr<detail::RenderPass> makePassForSlot(RenderPassSlot slot)
{
    switch (slot) {
    case RenderPassSlot::Shadow:
        return std::make_unique<detail::ShadowPass>();
    case RenderPassSlot::ForwardOpaque:
        return std::make_unique<detail::ForwardOpaquePass>();
    case RenderPassSlot::Transparent:
        return std::make_unique<detail::TransparentPass>();
    case RenderPassSlot::PostProcess:
        return std::make_unique<detail::PostProcessPass>();
    case RenderPassSlot::UI:
        return std::make_unique<detail::UIPass>();
    // §P5 B3 (2026-07-22) — Deferred-only slots. Both shells are
    // empty (B2 GBufferPass empty, B3 LightingPass empty). Real GPU
    // work lands in B4 / B5. Until then they Noop-gate on adapter
    // state and return 0 draws. Default `_enabled = true` from
    // RenderPass base; `applyPipelineDesc` never `setEnabled(false)`
    // — semantics of "Forward path doesn't see this pass" comes
    // from the factory omitting the slot, not from a runtime gate.
    //
    // §Skybox0 (2026-07-23) — Skybox slot maps to SkyboxPass. Only
    // mounted when `makeDeferred()` (or a custom desc) includes
    // the Skybox slot — Forward `makeDefault()` does NOT include
    // it, so Forward hosts see 0 behavior change.
    case RenderPassSlot::GBuffer:
        return std::make_unique<detail::GBufferPass>();
    case RenderPassSlot::Lighting:
        return std::make_unique<detail::LightingPass>();
    case RenderPassSlot::Skybox:
        return std::make_unique<detail::SkyboxPass>();
    // S1a (2026-07-23, short-term-plan §S1) — half-resolution
    // bright-extract pass. Default-enabled in both Forward and
    // Deferred pipelines. View 5 claim. K1 invariant #2: when
    // host keeps the default bloomStrength=0, the pass writes
    // zeros — visually identical to pre-S1 renders.
    case RenderPassSlot::BloomExtract:
        return std::make_unique<detail::BloomExtractPass>();
    // S1b (2026-07-23, short-term-plan §S1 sub-cut 2) — half-
    // resolution separable-Gaussian blur ping-pong. Default-
    // enabled in both Forward and Deferred pipelines. Views 12
    // (horizontal) + 13 (vertical) claim. K2 invariant #1: when
    // ctx.bloomExtractPass is absent (custom desc omits Extract)
    // or producer FBO is invalid, the pass early-returns 0 —
    // visually identical to pre-S1 renders.
    case RenderPassSlot::BloomBlur:
        return std::make_unique<detail::BloomBlurPass>();
    // S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — half-
    // resolution exponential depth-aware haze pass. Default-
    // enabled in both Forward and Deferred pipelines. View 14
    // claim (cutsheet §S4 view map lock). K3 invariant #2:
    // frame.hazeEnabled=false ⇒ execute() early-returns BEFORE
    // ensureFbo ⇒ no FBO allocation ⇒ zero cost when the host
    // has not opted in. Mirrors BloomExtractPass / BloomBlurPass
    // per-cutsheet slot-table reservation philosophy.
    case RenderPassSlot::DepthHaze:
        return std::make_unique<detail::DepthHazePass>();
    }
    return nullptr;
}

} // namespace

struct Renderer::Impl {
    detail::BGFXAdapter           adapter;
    ayt::shader::ShaderResourcePool    shaderPool;
    // Product default = RenderPipelineDesc::makeDefault()
    // (Shadow → FO → Transparent → PostProcess → UI), Shadow enabled
    // by default (E5 §5.4, 2026-07-22). Hosts that want to opt out
    // pass a custom desc that omits the Shadow slot.
    //
    // §P5 B1 (2026-07-22) — `path` field plumbing only: Forward
    // default, Deferred opt-in via `makeDeferred()`. Actual
    // Deferred dispatch lands in B3.
    // §P5 B2 (2026-07-22) — `GBufferPass` empty shell wired into
    // the pipeline plumbing. Real MRT GPU work lands in B4 (new
    // BGFXAdapter::createGbufferFrameBuffer helper per docs/pass-
    // lessons-from-deferred.md §5.2); B5 LightingPass will consume
    // it via PassExecContext::gbufferPass.
    // §P5 B3 (2026-07-22) — Forward/Deferred path selection now
    // real (factory-layer per docs/pass-lessons-from-deferred.md
    // §1.3 + E5 "omit slot = opt out" philosophy).
    // `makeDefault()` still returns 5-slot Forward unchanged;
    // `makeDeferred()` now returns 6-slot Deferred (Shadow +
    // GBuffer + Lighting + Transparent + PostProcess + UI).
    // ForwardOpaque is OMITTED from Deferred list per cutsheet
    // §4.1 red line #4 — never re-rendered after Lighting.
    // `applyPipelineDesc` for-loop 0 changes (走工厂层决策);
    // `RenderPassSlot::GBuffer` / `::Lighting` enum values + the
    // matching `makePassForSlot` switch cases land in this PR.
    // B4 / B5 wire real GPU on top.
    detail::RenderPipeline        pipeline;
    RenderPipelineDesc            pipelineDesc = RenderPipelineDesc::makeDefault();

    // §F2 (2026-07-24, mid-term FG MVP) — FrameGraph owns the
    // post-process chain transient resources (BloomBright /
    // BloomBlurA/B / HazeHalf). Lives here (Impl member) so its
    // lifetime matches the Renderer; borrowed via
    // PassExecContext::frameGraph for each render() call.
    // Constructed with the adapter reference (it queries the
    // adapter's isInitialized/isNoopBackend per-frame).
    detail::FrameGraph            frameGraph{adapter};

    detail::RenderResourceManager resources;
    detail::DebugOverlay          debugOverlay;
    InitDesc                      initDesc{};
    bool                          shaderPoolReady = false;
    uint32_t                      lastDrawCalls   = 0;
    uint32_t                      lastSceneItems  = 0;
    std::string                   pendingScreenshotBase;
    std::string                   finalizeScreenshotBase;

    ayt::math::Float4x4           mainView        = ayt::math::Float4x4::identity();
    ayt::math::Float4x4           mainProjection  = ayt::math::Float4x4::identity();
    // §Skybox0 (2026-07-23) — host-supplied Skybox DataSource
    // borrowed pointer. Default nullptr = no sky mounted (Forward
    // default). When the host calls `Renderer::setSkySource(&sky)`,
    // the renderer reads `sky.equirect` each frame via the borrowed
    // pointer; the SkySource instance must outlive render(). Mirror
    // `sceneLights` borrowed-ptr shape; same lifetime contract.
    const ayt::render::SkySource*   skySource   = nullptr;
    // §P5.5 D (2026-07-23) — host-uploaded cube map handle for IBL
    // MVP. Cached here so the host-facing getter (`skySourceCube()`)
    // can read it back, AND so setSkySourceCube can forward to the
    // SkyboxPass via `pipeline.findPass("Skybox")` → setCubeTexture
    // (mirror equirect: equirect lives in SkySource; cube lives on
    // the SkyboxPass producer state so execute() can read it
    // without touching PassExecContext / FrameContext — cutsheet
    // §5.3 red lines 0 ctx field additions per cut). Default =
    // TextureHandle{} (invalid) = cube path inactive =
    // pre-D byte-equivalent flat ambient + flat equirect backdrop.
    // Clear-by-invalid: the host passes `TextureHandle{}` to revert
    // to the equirect path.
    ayt::render::TextureHandle      skyCubeTexture{};
    // §P5 B4c (2026-07-22) — previous-frame view/projection cached on
    // Renderer::Impl. GBufferPass samples these for per-pixel motion
    // vectors (gl_FragData[2] = NDC half-range encoded displacement
    // between current-frame clipPos and previous-frame clipPos, scaled
    // by 0.5 + 0.5 into RGBA8 [0,1]).
    //
    // Lifecycle: end-of-frame commit in `Renderer::render()` — AFTER
    // executeAll(). Beginning-of-render swap would alias prev with
    // the brand-new mainView (host calls setMainCamera(...) BEFORE
    // render(), see setMainCamera at line 916), making every pixel's
    // motion vector collapse to the constant 0.5 vec2 — the host's
    // "I've moved the camera" signal would be invisible. End-of-frame
    // commit means the next render() reads this frame's mainView as
    // prev, which is the correct temporal-sampling window.
    //
    // First frame: prev = identity (never updated). B4c documents
    // this as "garbage motion on frame 0, acceptable for B7+ TAA
    // consumer because TAA is a multi-frame accumulator and one
    // noisy seed fades into the history". Future cuts (B7+ TAA)
    // can substitute a sane prev (e.g., identity-encoded freeze)
    // if they need deterministic frame-0 output.
    //
    // Cutsheet §5.3 red-line compliance: FrameContext is NOT touched
    // (sizeof(FrameContext) invariant). PassExecContext is NOT
    // touched (≤1 borrowed-ptr field per cut budget not consumed).
    // The data flows strictly one-way: render() writes prev*, GBufferPass
    // reads via `setPrevViewProj()` push from render() then read inside
    // execute(). No consumer mutates back.
    ayt::math::Float4x4           prevMainView       = ayt::math::Float4x4::identity();
    ayt::math::Float4x4           prevMainProjection = ayt::math::Float4x4::identity();
    ayt::math::FVector3           mainCameraPosition = ayt::math::FVector3(0.0f, 0.0f, 4.0f);
    ayt::math::FVector3           directionalLightDir = ayt::math::FVector3(0.3f, -0.8f, -0.4f);
    ayt::math::FVector3           directionalLightColor = ayt::math::FVector3(1.0f, 1.0f, 1.0f);
    bool                          shadowPcfEnabled = true;

    void applyShadowQualityKnobs()
    {
        if (detail::RenderPass* shadowPass = pipeline.findPass("Shadow")) {
            static_cast<detail::ShadowPass*>(shadowPass)->setPcfEnabled(shadowPcfEnabled);
        }
    }

    // R5+ (Phase PostProcess) — per-host post-process knobs.
    // Defaults = no effect (bloom=0, exposure=1, ripple=0, tonemap=None).
    float                          postProcessBloomStrength  = 0.0f;
    float                          postProcessExposure       = 1.0f;
    float                          postProcessGamma          = 2.2f;
    // §P5.5 D — IBL ambient cube strength (.x uploaded as vec4).
    float                          ambientStrength           = 0.6f;
    detail::FrameContext::TonemapMode postProcessTonemapMode = detail::FrameContext::TonemapMode::None;

    // §S4d — DepthHaze host knobs (FrameContext defaults stay off).
    bool                           depthHazeEnabled  = false;
    float                          depthHazeStrength = 0.0f;
    float                          depthHazeDensity  = 0.02f;
    ayt::math::FVector3            depthHazeColor    =
        ayt::math::FVector3(0.55f, 0.65f, 0.78f);

    // P4.2 (§P4, 2026-07-22) — global shadow receiver bias in ndc01
    // units. Mirrored into FrameContext::shadowBias each render so
    // tryBindShadowSampler() (ForwardOpaquePass + TransparentPass
    // call sites) uploads it into every receiver material's
    // `shadowBias` uniform. Default 0.003f matches the Phoskia
    // receiver property default + ShadowSettings::kBiasDefault;
    // existing shaders render identically without host action.
    float                          shadowBias               = 0.003f;

    // §P5 B7+ (2026-07-22) — host-supplied multi-light DataSource
    // (ayt::render::SceneLights). Borrowed pointer (host owns the
    // storage); null = host did not call setSceneLights ⇒
    // LightingPass falls back to the B5 single-light path
    // (FrameContext::lightDirection / lightColor).
    //
    // Lifetime contract: pointer must outlive render(). Default
    // nullptr matches existing single-light host patterns; no
    // regression. Future Editor Play with multi-light state fills
    // this in host app code (e.g. AppState::lights populate by
    // editor / game logic; renderer just consumes).
    const ayt::render::SceneLights* sceneLights = nullptr;

    // P0 (2026-07-20) — wall-clock origin for FrameContext.timeSeconds.
    // std::chrono::steady_clock is monotonic (immune to wall-clock
    // adjustments) which is what R5+ post-process effects (time-of-day
    // color grading, bloom pulse) need. Set on the first successful
    // initialize(); consumed by Renderer::render into frame.timeSeconds.
    std::chrono::steady_clock::time_point renderClockOrigin{};
    bool  renderClockPaused = false;
    float renderClockFrozenSeconds = 0.0f;
    bool  hasSimulationTime = false;
    float simulationTimeSeconds = 0.0f;

    uint16_t                      viewportX = 0;
    uint16_t                      viewportY = 0;
    uint16_t                      viewportW = 0;
    uint16_t                      viewportH = 0;

    // -1 = normal (3D on view 0). >=0 = composite scene view (usually 1).
    int                           compositeSceneViewId = -1;

    // P2 (PR-D, 2026-07-20) — shared scene color/depth FBO that
    // ForwardOpaquePass + TransparentPass draw into. PostProcessPass
    // samples attach0 as its scene color input. Lifetime: built lazily
    // in render() once the adapter is initialized + the viewport is
    // non-zero, rebuilt on resize(), destroyed in shutdown().
    // INVALID = "no scene RT", which is the test-path default (Noop
    // backend ⇒ ensureSceneFbo never produces a valid handle).
    bgfx::FrameBufferHandle        sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    uint16_t                       sceneFboW = 0;
    uint16_t                       sceneFboH = 0;

    // P2 — ensure sceneFbo matches the current viewport. Returns
    // BGFX_INVALID_HANDLE when the adapter is uninitialized or size=0
    // (so callers can no-op cleanly).
    bgfx::FrameBufferHandle ensureSceneFbo();

    // Rebuild pipeline.passes from pipelineDesc. Preserves UI backend.
    void applyPipelineDesc(const RenderPipelineDesc& desc);

    // §5.5 cleanup (2026-07-22) — `lastFrameShadowFbo` cache removed.
    // It lived under #if AY_F1_DIAG_FRAME_SHADOW and was used only by
    // the now-retired F1 diagnostic path. E5 ships default-on Shadow
    // without that diagnostic; consumers that want the active shadow
    // FBO should ask the ShadowPass producer directly via
    // PassExecContext::shadowPass->shadowFbo() (or skip the cache
    // entirely since the per-frame FBO lookup is already O(1)).

    Impl()
        : resources(adapter, shaderPool)
    {
        // E5 (§5.4, 2026-07-22): makeDefault() mounts Shadow
        // enabled (not disabled) — pre-E4 the canonical default
        // disabled Shadow to keep the 0-behavior-change baseline,
        // pre-E5 the canonical default disabled Shadow under the
        // E4 std::equal override. E5 ships "default-on Shadow"
        // because (a) ShadowPass::execute Noop-gates cleanly
        // (early-return 0 draw on Noop / uninitialized adapters),
        // (b) tryBindShadowSampler already no-ops when the shadow
        // FBO is invalid or the shader binding is missing, and
        // (c) §5.3 still forbids default-on Shadow *combined with*
        // a Light struct or FrameContext shadow writeback — both
        // DIAG flags remain OFF.
        applyPipelineDesc(RenderPipelineDesc::makeDefault());
    }
};

void Renderer::Impl::applyPipelineDesc(const RenderPipelineDesc& desc)
{
    RenderPipelineDesc resolved = desc.passes.empty()
                                      ? RenderPipelineDesc::makeDefault()
                                      : desc;

    UIRenderBackend* retainedUi = nullptr;
    if (detail::RenderPass* uiPass = pipeline.findPass("UI")) {
        retainedUi = static_cast<detail::UIPass*>(uiPass)->backend();
    }

    if (detail::RenderPass* shadowPass = pipeline.findPass("Shadow")) {
        if (adapter.isInitialized()) {
            static_cast<detail::ShadowPass*>(shadowPass)
                ->destroyResources(adapter);
        }
    }

    // §P5 B4a (2026-07-22) — GBuffer destroyResources mirror (mirror
    // Shadow destroy block above). Cutsheet §5.2 + P2 sceneFbo closure
    // pattern. Runs BEFORE pipeline.clear() so the FBO handle survives
    // the rebuild (matches Shadow mirror — bgfx handle table rotates
    // between pipeline rebuilds, post-clear destroy would race with
    // new-pass FBO allocation).
    if (detail::RenderPass* gbufferPass = pipeline.findPass("GBuffer")) {
        if (adapter.isInitialized()) {
            static_cast<detail::GBufferPass*>(gbufferPass)->destroyResources(adapter);
        }
    }

    // §P5 B5 (2026-07-22) — LightingPass destroyResources mirror
    // (mirror GBuffer destroy block above). LightingPass owns a
    // 1× RGBA8 LightingOutput FBO + fullscreen triangle VB/IB +
    // Phoskia Lighting program; all three must be released BEFORE
    // pipeline.clear() for the same handle-rotation reason as
    // Shadow/GBuffer. cutsheet `pass-lessons-from-deferred.md:151,
    // 161, 169` lock the LightingOutput FBO lifetime to the
    // LightingPass owner.
    if (detail::RenderPass* lightingPass = pipeline.findPass("Lighting")) {
        if (adapter.isInitialized()) {
            static_cast<detail::LightingPass*>(lightingPass)->destroyResources(adapter);
        }
    }

    // §Skybox0 (2026-07-23) — SkyboxPass destroyResources mirror
    // (mirror LightingPass destroy block above). SkyboxPass owns a
    // 1× RGBA8 SkyOutput FBO + fullscreen triangle VB/IB + Phoskia
    // Skybox program; all three must be released BEFORE
    // pipeline.clear() for the same handle-rotation reason.
    if (detail::RenderPass* skyboxPass = pipeline.findPass("Skybox")) {
        if (adapter.isInitialized()) {
            static_cast<detail::SkyboxPass*>(skyboxPass)->destroyResources(adapter);
        }
    }

    // S1a (2026-07-23, short-term-plan §S1) — BloomExtractPass
    // destroyResources mirror (mirror SkyboxPass destroy block
    // above). BloomExtractPass owns a half-resolution RGBA8 FBO
    // (no depth) + fullscreen-triangle VB/IB + Phoskia extract
    // program; all three must be released BEFORE pipeline.clear()
    // for the same handle-rotation reason.
    if (detail::RenderPass* bloomExtractPass = pipeline.findPass("BloomExtract")) {
        if (adapter.isInitialized()) {
            static_cast<detail::BloomExtractPass*>(bloomExtractPass)->destroyResources(adapter);
        }
    }

    // S1b (2026-07-23, short-term-plan §S1 sub-cut 2) —
    // BloomBlurPass destroyResources mirror (mirror
    // BloomExtractPass destroy block above). BloomBlurPass owns
    // two half-resolution RGBA8 ping-pong FBOs (no depth) +
    // fullscreen-triangle VB/IB + Phoskia blur program; all four
    // must be released BEFORE pipeline.clear() for the same
    // handle-rotation reason.
    if (detail::RenderPass* bloomBlurPass = pipeline.findPass("BloomBlur")) {
        if (adapter.isInitialized()) {
            static_cast<detail::BloomBlurPass*>(bloomBlurPass)->destroyResources(adapter);
        }
    }

    // S4b (2026-07-23, short-term-plan §S4 sub-cut 2) —
    // DepthHazePass destroyResources mirror (mirror BloomExtractPass /
    // BloomBlurPass destroy blocks above). DepthHazePass owns a
    // half-resolution RGBA8 FBO (no depth, lazy-ensured only when
    // hazeEnabled=true + hazeStrength>0) + fullscreen-triangle
    // VB/IB + Phoskia haze program; all three must be released
    // BEFORE pipeline.clear() for the same handle-rotation reason.
    // When the host has not opted in (hazeEnabled=false ⇒ ensureFbo
    // never ran ⇒ _fbo is invalid), destroyResources is a no-op
    // (K3 invariant #2 holds: zero allocation ⇒ zero release work).
    if (detail::RenderPass* depthHazePass = pipeline.findPass("DepthHaze")) {
        if (adapter.isInitialized()) {
            static_cast<detail::DepthHazePass*>(depthHazePass)->destroyResources(adapter);
        }
    }

    pipeline.clear();
    // E5 (§5.4, 2026-07-22): default pipeline now mounts EVERY slot
    // at its RenderPass base default (_enabled == true), Shadow
    // included. The E4 "canonical-default ⇒ Shadow disabled" override
    // is removed — that std::equal detection was a no-op distinction
    // (makeDefault() and makeForwardWithShadows() were byte-identical)
    // and the resulting behavior contradicted the E4.4 test comment.
    // No FrameContext shadow slot / Light struct is introduced
    // (§5.3 red lines): Shadow runs, but writes nothing back through
    // FrameContext.
    for (const RenderPassSlot slot : resolved.passes) {
        if (auto pass = makePassForSlot(slot)) {
            pipeline.addPass(std::move(pass));
        }
    }
    pipelineDesc = std::move(resolved);

    if (retainedUi != nullptr) {
        if (detail::RenderPass* uiPass = pipeline.findPass("UI")) {
            static_cast<detail::UIPass*>(uiPass)->setBackend(retainedUi);
        }
    }

    applyShadowQualityKnobs();
}

Renderer::Renderer() : _impl(std::make_unique<Impl>())
{
}

Renderer::~Renderer()
{
    shutdown();
}

Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

bool Renderer::initialize(const InitDesc& desc)
{
    if (!_impl) {
        _impl = std::make_unique<Impl>();
    }

    if (_impl->adapter.isInitialized()) {
        return true;
    }

    detail::BGFXInitParams bgfxParams;
    bgfxParams.nativeWindowHandle = desc.windowHandle;
    bgfxParams.width              = desc.width;
    bgfxParams.height             = desc.height;
    bgfxParams.vsync              = desc.vsync;
    bgfxParams.backend            = desc.backend;
    bgfxParams.msaa               = desc.msaa;
    if (const char* msaaEnv = std::getenv("AY_MSAA")) {
        bgfxParams.msaa = static_cast<uint32_t>(std::atoi(msaaEnv));
    }

    if (!_impl->adapter.initialize(bgfxParams)) {
        return false;
    }

    _impl->initDesc        = desc;
    _impl->initDesc.msaa   = bgfxParams.msaa;
    _impl->shadowPcfEnabled = desc.shadowPcf;
    if (const char* pcfEnv = std::getenv("AY_SHADOW_PCF")) {
        _impl->shadowPcfEnabled = !(pcfEnv[0] == '\0' || pcfEnv[0] == '0');
    }
    _impl->initDesc.shadowPcf = _impl->shadowPcfEnabled;
    _impl->applyShadowQualityKnobs();
    std::fprintf(stderr,
                 "[Renderer] quality msaa=%u shadowPcf=%d "
                 "(override via AY_MSAA / AY_SHADOW_PCF, or Renderer setters)\n",
                 static_cast<unsigned>(_impl->adapter.msaaSampleCount()),
                 _impl->shadowPcfEnabled ? 1 : 0);
    _impl->viewportW       = static_cast<uint16_t>(desc.width);
    _impl->viewportH       = static_cast<uint16_t>(desc.height);
    _impl->viewportX       = 0;
    _impl->viewportY       = 0;
    _impl->shaderPoolReady = detail::configureShaderPool(_impl->shaderPool);
    if (_impl->shaderPoolReady) {
        _impl->shaderPool.resolvePlatformFromRenderer();
    }
    _impl->debugOverlay.setEnabled(desc.enableDebugOverlay);
    // P0 — start the wall-clock origin for FrameContext.timeSeconds.
    // First initialize() = origin 0; subsequent renders see elapsed
    // seconds since this point. initialize() is idempotent (early
    // return if adapter already initialized) so the second call won't
    // re-stamp the origin; render() guards on "origin not yet set".
    _impl->renderClockOrigin = std::chrono::steady_clock::now();
    return true;
}

void Renderer::shutdown()
{
    if (!_impl) {
        return;
    }

    // P2 (PR-D) — release the scene FBO before tearing down the
    // adapter. Mirrors the PostProcessPass FBO destroy pattern:
    // bgfx::destroy on a stale handle after bgfx::shutdown() is the
    // documented safe sequence (Adapter's destroy() gates on
    // isInitialized but doesn't tear down the bgfx handle mapping —
    // it just calls bgfx::destroy, which is a no-op on the dying
    // handle map). Doing it here guarantees Impl ctor / shutdown
    // pairs are balanced across the ProcessBgfxAlive sticky window.
    if (detail::BGFXAdapter::isValid(_impl->sceneFbo)) {
        _impl->adapter.destroy(_impl->sceneFbo);
        _impl->sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _impl->sceneFboW = 0;
        _impl->sceneFboH = 0;
    }

    _impl->resources.shutdown();
    _impl->shaderPool.shutdown();
    _impl->adapter.shutdown();
    _impl->shaderPoolReady = false;
}

bool Renderer::isInitialized() const noexcept
{
    return _impl && _impl->adapter.isInitialized();
}

void Renderer::beginFrame(const ClearDesc& clear)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }
    _impl->compositeSceneViewId = -1;
    _impl->debugOverlay.onBeginFrame();
    _impl->adapter.beginFrame();
    _impl->adapter.setViewClear(detail::ForwardOpaquePass::kMainViewId, clear);
}

void Renderer::beginCompositeFrame(const ClearDesc& clear, uint16_t fbWidth, uint16_t fbHeight)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }
    _impl->debugOverlay.onBeginFrame();

    // View 0: full-window clear only (never shrink this rect to the 3D hole).
    _impl->adapter.setViewRect(0, 0, 0, fbWidth, fbHeight);
    _impl->adapter.setViewClear(0, clear);
    _impl->adapter.beginFrame(); // touch(0) so the clear runs

    // View 3: 3D into the scene FBO / panel hole (see UIRenderBackend::kViewId map).
    // Must not clear the backbuffer here (would wipe chrome); FO clears
    // the offscreen scene FBO itself when binding it.
    // View 2 is reserved for ShadowPass resolve blit.
    _impl->compositeSceneViewId = 3;
    _impl->adapter.setViewClearNone(3);
}

void Renderer::render(const RenderScene& scene)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }

    _impl->lastSceneItems = static_cast<uint32_t>(scene.items().size());
    _impl->lastDrawCalls  = 0;

    if (scene.empty()) {
        return;
    }

    detail::FrameContext frame;
    frame.view             = _impl->mainView;
    frame.projection       = _impl->mainProjection;
    frame.cameraPosition   = _impl->mainCameraPosition;
    frame.lightDirection   = _impl->directionalLightDir.normalize();
    frame.lightColor       = _impl->directionalLightColor;
    // P0 — wall-clock seconds since Renderer::initialize(). Field
    // was 0 by default before this assignment; existing tests that
    // built FrameContext manually and checked field-by-field still
    // see 0.0f. R5+ post-process will read this into a `u_time`
    // uniform.
    if (_impl->hasSimulationTime) {
        frame.timeSeconds = _impl->simulationTimeSeconds;
        _impl->renderClockFrozenSeconds = frame.timeSeconds;
    } else if (_impl->renderClockOrigin.time_since_epoch().count() != 0) {
        if (_impl->renderClockPaused) {
            frame.timeSeconds = _impl->renderClockFrozenSeconds;
        } else {
            const auto now = std::chrono::steady_clock::now();
            const std::chrono::duration<float> elapsed = now - _impl->renderClockOrigin;
            frame.timeSeconds = elapsed.count();
            _impl->renderClockFrozenSeconds = frame.timeSeconds;
        }
    }
    // R5+ — host-configured post-process knobs. Rendered every frame
    // even when the value hasn't changed because the FrameContext is
    // stack-local; cost is negligible (3 floats + 1 byte enum).
    frame.bloomStrength    = _impl->postProcessBloomStrength;
    frame.exposure         = _impl->postProcessExposure;
    frame.gamma            = _impl->postProcessGamma;
    frame.tonemapMode      = _impl->postProcessTonemapMode;
    // §S4d — DepthHaze knobs (Editor / host). Defaults keep haze off.
    frame.hazeEnabled      = _impl->depthHazeEnabled;
    frame.hazeStrength     = _impl->depthHazeStrength;
    frame.hazeDensity      = _impl->depthHazeDensity;
    frame.hazeColor        = _impl->depthHazeColor;
    // P4.2 (§P4, 2026-07-22) — global shadow receiver bias copied
    // into FrameContext each frame; tryBindShadowSampler reads it.
    frame.shadowBias       = _impl->shadowBias;

    // §5.5 cleanup (2026-07-22) — the F1-diagnostic FrameContext
    // shadow-writeback block (lastFrameShadowFbo cache → frame.shadowFboIdx
    // / lightViewProj / lightIndex) is removed. That path was the §5.5
    // PR-F1' C' forbidden combo (FrameContext shadow writeback + default-on
    // Shadow). E5 ships default-on Shadow without the writeback, and the
    // hosts consume the producer's FBO + light-view-proj via the bypass
    // getter on PassExecContext::shadowPass (see PR-F2 / shadow-pass.md).

    const uint8_t viewId = _impl->compositeSceneViewId >= 0
                               ? static_cast<uint8_t>(_impl->compositeSceneViewId)
                               : detail::ForwardOpaquePass::kMainViewId;

    _impl->lastDrawCalls = 0;

    // Scene FBO for FO/Transparent → PostProcess sample → backbuffer blit.
    // Editor composite also uses this path: FO draws into a panel-sized
    // offscreen RT (rect 0,0,w,h); PostProcessPass blits to the Game View
    // hole (vx,vy,w,h) on view 4. Previously composite forced INVALID
    // FBO (3D direct to backbuffer) which made PostProcess early-out —
    // so Editor never saw any post filter (ripple included).
    const bgfx::FrameBufferHandle sceneFbo = _impl->ensureSceneFbo();

    // §P5 B4b (2026-07-22) — broadcast viewport size to GBufferPass
    // before dispatch so its execute() can ensure() the 4-attach
    // MRT FBO at the correct W×H. Mirror the sceneFbo / viewportW
    // wiring above (size is the same panel rect for the Deferred
    // path). Skipped when GBuffer isn't in the configured pipeline
    // (Forward path) — cutsheet §4.1 red line #4.
    if (detail::RenderPass* gbufferSlot = _impl->pipeline.findPass("GBuffer")) {
        if (_impl->viewportW > 0 && _impl->viewportH > 0) {
            static_cast<detail::GBufferPass*>(gbufferSlot)
                ->setGbufferSize(static_cast<uint16_t>(_impl->viewportW),
                                 static_cast<uint16_t>(_impl->viewportH));
        }
        // §P5 B4c (2026-07-22) — push previous-frame view/projection
        // into GBufferPass so execute() can build prevViewProj =
        // prevProj * prevView (P×V same-order as `setViewTransform`
        // + `viewProjectionMatrix` builtin ordering, mirror docs/
        // pass-lessons-from-shadow.md §3.1 "CPU 也要 P×V 与
        // setViewTransform 同序") and upload it as `u_prevViewProj`.
        // First frame: prev = identity ⇒ garbage motion ⇒ B7+ TAA
        // consumer tolerates (B4c cutsheet decision). Repeated
        // calls per frame are idempotent — GBufferPass stores
        // locally, no GPU work until execute() runs.
        auto* gbufferTyped = static_cast<detail::GBufferPass*>(gbufferSlot);
        gbufferTyped->setPrevViewProj(_impl->prevMainView,
                                      _impl->prevMainProjection);
    }

    // §P5 B5 (2026-07-22) — broadcast viewport size to LightingPass
    // before dispatch so its execute() can ensure() the 1× RGBA8
    // LightingOutput FBO at the correct W×H. Mirror the GBuffer
    // setGbufferSize block above (same viewport rect). Skipped
    // when Lighting isn't in the configured pipeline (Forward path)
    // — cutsheet §4.1 red line #4.
    if (detail::RenderPass* lightingSlot = _impl->pipeline.findPass("Lighting")) {
        auto* lighting = static_cast<detail::LightingPass*>(lightingSlot);
        if (_impl->viewportW > 0 && _impl->viewportH > 0) {
            lighting->setOutputSize(static_cast<uint16_t>(_impl->viewportW),
                                    static_cast<uint16_t>(_impl->viewportH));
        }
        // §P5.5 D — host IBL ambient knob (survives configurePipeline).
        lighting->setAmbientStrength(_impl->ambientStrength);
    }

    // §Skybox0 (2026-07-23) — broadcast viewport size to SkyboxPass
    // before dispatch so its execute() can ensure() the 1× RGBA8
    // SkyOutput FBO at the correct W×H. Mirror the LightingPass
    // setOutputSize block above (same viewport rect). Skipped
    // when Skybox isn't in the configured pipeline (Forward
    // default) — `makeDefault()` does not include the Skybox slot,
    // so Forward hosts see 0 behavior change (cutsheet §5.3 red
    // line #4). The host's `setSkySource()` call is still safe to
    // make on Forward — the borrowed pointer is simply ignored
    // because `ctx.skyboxPass == nullptr` and `ctx.skySource` is
    // never read.
    if (detail::RenderPass* skyboxSlot = _impl->pipeline.findPass("Skybox")) {
        if (_impl->viewportW > 0 && _impl->viewportH > 0) {
            static_cast<detail::SkyboxPass*>(skyboxSlot)
                ->setOutputSize(static_cast<uint16_t>(_impl->viewportW),
                                static_cast<uint16_t>(_impl->viewportH));
        }
    }

    // P1 (PR-C, 2026-07-20): build the PassExecContext once per frame
    // and hand it to RenderPipeline::executeAll. Every enabled pass
    // reads from the same context. Adding new per-frame state (e.g.
    // a ShadowMap slot in P3, scene-RT handles in P2) means adding a
    // field to PassExecContext, NOT a new execute() arg.
    //
    // PR-F2 / pipeline config — when Shadow is in the configured
    // pipeline, hand FO/Transparent a non-owning pointer so
    // tryBindShadowSampler can upload u_lightViewProj + bind
    // shadowMap. Absent Shadow ⇒ nullptr (no upload, no sampler).
    const detail::ShadowPass* shadowPassPtr = nullptr;
    if (detail::RenderPass* shadowSlot = _impl->pipeline.findPass("Shadow")) {
        shadowPassPtr = static_cast<const detail::ShadowPass*>(shadowSlot);
    }

    // §P5 B2 (2026-07-22) — when GBuffer is in the configured
    // pipeline, hand downstream passes (B5 LightingPass, future
    // B7+ multi-light consumers) a non-owning pointer so they can
    // read the GBuffer MRT attachments without FrameContext
    // writeback (§5.3 red line). Today the GBufferPass shell is
    // empty — gbufferFbo() returns BGFX_INVALID_HANDLE and the
    // shell's execute() Noop-gates — so consumers receive a
    // present-but-empty signal (same shape as ShadowPass on
    // Noop). Absent GBuffer ⇒ nullptr.
    const detail::GBufferPass* gbufferPassPtr = nullptr;
    if (detail::RenderPass* gbufferSlot = _impl->pipeline.findPass("GBuffer")) {
        gbufferPassPtr = static_cast<const detail::GBufferPass*>(gbufferSlot);
    }

    // §P5 B3 (2026-07-22) — when Lighting is in the configured
    // pipeline (Deferred path), hand downstream passes (future
    // B7+ multi-light consumers; B5's LightingPass itself is the
    // dispatch endpoint and doesn't read this back) a non-owning
    // pointer so they can read the lighting output FBO without
    // FrameContext writeback (§5.3 red line). Today the LightingPass
    // shell is empty — lightingFbo() returns BGFX_INVALID_HANDLE
    // and the shell's execute() Noop-gates — so consumers receive
    // a present-but-empty signal (same shape as GBufferPass /
    // ShadowPass on Noop). Absent Lighting ⇒ nullptr.
    const detail::LightingPass* lightingPassPtr = nullptr;
    if (detail::RenderPass* lightingSlot = _impl->pipeline.findPass("Lighting")) {
        lightingPassPtr = static_cast<const detail::LightingPass*>(lightingSlot);
    }

    // §Skybox0 (2026-07-23) — borrowed pointer to the SkyboxPass in
    // the pipeline. nullptr when the host did not opt in via
    // `configurePipeline(makeDeferred())` (Forward / no skybox path).
    const detail::SkyboxPass* skyboxPassPtr = nullptr;
    if (detail::RenderPass* skyboxSlot = _impl->pipeline.findPass("Skybox")) {
        skyboxPassPtr = static_cast<const detail::SkyboxPass*>(skyboxSlot);
    }

    // §S1b BloomBlur (2026-07-23) — borrowed pointer to the
    // BloomExtractPass in the pipeline. nullptr when the host
    // built a custom desc that omitted the BloomExtract slot
    // (cutsheet §S1 "omit slot = opt out"). BloomBlurPass reads
    // the producer's half-res FBO through this pointer; absent
    // ⇒ BloomBlurPass early-returns 0 (visually identical to
    // bloomStrength=0 default). Mirrors the skyboxPassPtr /
    // lightingPassPtr / gbufferPassPtr / shadowPassPtr shape
    // (lifetime contract: pointer must remain valid for the
    // duration of pipeline::executeAll(ctx)).
    const detail::BloomExtractPass* bloomExtractPassPtr = nullptr;
    if (detail::RenderPass* bloomExtractSlot = _impl->pipeline.findPass("BloomExtract")) {
        bloomExtractPassPtr = static_cast<const detail::BloomExtractPass*>(bloomExtractSlot);
    }

    // §S1c (2026-07-23, short-term-plan §S1 sub-cut 3) — borrowed
    // pointer to the BloomBlurPass in the pipeline. nullptr when
    // the host built a custom desc that omitted the BloomBlur slot.
    // PostProcessPass reads `ctx.bloomBlurPass->pongFbo()` (RT0 of
    // the vertically-blurred result) and binds it as the second
    // sampler on the fullscreen-triangle composite draw, replacing
    // the pre-S1 fake `raw + raw*bloomStrength` shader hack with
    // the real `raw + sample(bloomTexture, uv) * bloomStrength`
    // composite. nullptr ⇒ PostProcessPass falls back to binding
    // sceneColor on slot 1 (no GLSL sampler-not-set warning;
    // FS branchless composite collapses to `raw * (1 + 0) = raw`
    // — byte-equivalent to a zero-bloom pipeline). Mirrors
    // bloomExtractPassPtr above.
    const detail::BloomBlurPass* bloomBlurPassPtr = nullptr;
    if (detail::RenderPass* bloomBlurSlot = _impl->pipeline.findPass("BloomBlur")) {
        bloomBlurPassPtr = static_cast<const detail::BloomBlurPass*>(bloomBlurSlot);
    }

    // §S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — borrowed
    // pointer to the DepthHazePass in the pipeline. nullptr when the
    // host built a custom desc that omitted the DepthHaze slot
    // (cutsheet §S4 "omit slot = opt out"). PostProcessPass (after
    // §S4c) reads `ctx.depthHazePass->halfResFbo()` (RT0 of the haze
    // result) and binds it as the `hazeTexture` sampler on the
    // fullscreen composite draw. nullptr ⇒ PostProcessPass falls
    // back to binding sceneColor on slot 2 (FS branchless composite
    // collapses to `raw * (1 - 0) = raw` — byte-equivalent to
    // hazeEnabled=false). Mirrors bloomBlurPassPtr above.
    const detail::DepthHazePass* depthHazePassPtr = nullptr;
    if (detail::RenderPass* depthHazeSlot = _impl->pipeline.findPass("DepthHaze")) {
        depthHazePassPtr = static_cast<const detail::DepthHazePass*>(depthHazeSlot);
    }

    // §P5.5 C (2026-07-23) — wire the per-frame SceneLights ref
    // into ShadowPass so its multi-caster loop can read
    // `lights[i].castShadow` and build the per-slot LVP matrices.
    // Mirror pattern: the SkyboxPass receives its cube texture
    // via `findPass("Skybox")→setCubeTexture(...)` (not via a
    // borrowed ptr) because the cube handle is a Resource (host-
    // owned TextureHandle). ShadowPass needs the borrowed SceneLights
    // ref because it reads castShadow flags per-frame — host owns
    // the SceneLights instance lifetime (mirror ctx.perLightShadows
    // borrowed ptr lifetime contract).
    //
    // When `_impl->sceneLights == nullptr` (host on Forward path /
    // never called setSceneLights), ShadowPass falls back to the
    // pre-C single key-light caster (pre-C byte-equivalent).
    if (detail::RenderPass* shadowSlot = _impl->pipeline.findPass("Shadow")) {
        static_cast<detail::ShadowPass*>(shadowSlot)->setSceneLightsRef(
            _impl->sceneLights);
    }

    // §F2 (2026-07-24, mid-term FG MVP) — FrameGraph is the
    // post-process chain resource pool. build-graph site lives
    // HERE (not in Pass::execute) so the compile pass sees the
    // full pass list before any Resolve() fires. F2 wires
    // BloomExtract only; F3-F5 migrate BloomBlur / DepthHaze /
    // PostProcess incrementally.
    //
    // bloomEnabled is the unified gate for the whole bloom
    // chain (extract + blur) — host's bloomStrength > 0 enables
    // it; otherwise the chain is fully culled at compile time
    // and no transient RTs are created (cutsheet §7 row 3).
    detail::FrameGraph& fg = _impl->frameGraph;
    const uint16_t fgW = _impl->viewportW;
    const uint16_t fgH = _impl->viewportH;
    fg.beginFrame(fgW, fgH);
    // Import SceneColor as an external resource (borrowed, not
    // owned). The physical source is decided per frame based on
    // which pipeline path is active: Deferred ⇒ Lighting output;
    // Forward ⇒ the renderer's sceneFbo.
    bgfx::FrameBufferHandle sceneColorHandle = sceneFbo;
    if (lightingPassPtr != nullptr
        && detail::BGFXAdapter::isValid(lightingPassPtr->lightingOutputFbo())) {
        sceneColorHandle = lightingPassPtr->lightingOutputFbo();
    }
    fg.importExternal(detail::FgResourceId::SceneColor, sceneColorHandle);

    const bool bloomEnabled = (frame.bloomStrength > 0.0f);
    if (bloomEnabled) {
        // F2 ships with the physical RT path still gated on FG
        // resolve() returning invalid (FG physical creation
        // deferred to F6). The compile-time declaration still
        // happens so enabled-vs-disabled visibility is correct.
        fg.addResource(detail::FgResourceId::BloomBright,
                       {bgfx::TextureFormat::RGBA8,
                        detail::FgTextureScale::Half,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addPass({"BloomExtract",
                    {detail::FgResourceId::SceneColor},
                    {detail::FgResourceId::BloomBright},
                    /*enabled=*/true});

        // §F3 (2026-07-24) — BloomBlur ping-pong targets. Two
        // distinct logical resources (BloomBlurA / BloomBlurB);
        // aliasing is forbidden by design — cutsheet §4 "BloomBlur
        // A/B 显式禁止 alias". The FG compile step keeps them in
        // separate physical RTs even when their FgTextureDesc
        // matches (the resolvePingPong() path through FgResourceId
        // does the bookkeeping). Both declared inside the
        // `bloomEnabled` gate so bloomStrength=0 ⇒ no transient
        // RTs allocated (K2 #1 invariant).
        fg.addResource(detail::FgResourceId::BloomBlurA,
                       {bgfx::TextureFormat::RGBA8,
                        detail::FgTextureScale::Half,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addResource(detail::FgResourceId::BloomBlurB,
                       {bgfx::TextureFormat::RGBA8,
                        detail::FgTextureScale::Half,
                        /*transient=*/true,
                        /*withDepth=*/false});
        // H pass (view 11): BloomBright → BloomBlurA.
        fg.addPass({"BloomBlurH",
                    {detail::FgResourceId::BloomBright},
                    {detail::FgResourceId::BloomBlurA},
                    /*enabled=*/true});
        // V pass (view 12): BloomBlurA → BloomBlurB.
        fg.addPass({"BloomBlurV",
                    {detail::FgResourceId::BloomBlurA},
                    {detail::FgResourceId::BloomBlurB},
                    /*enabled=*/true});
    }

    // §F4 (2026-07-24, mid-term FG MVP sub-cut 4) — DepthHazePass
    // HazeHalf target. Centralized `hazePassEnabled` gate:
    //   - frame.hazeEnabled (host knob)
    //   - frame.hazeStrength > 0 (otherwise fogFactor
    //     collapses to 0; no point allocating the RT)
    //   - gbufferPass != nullptr (Deferred-only MVP; the haze
    //     distance proxy reads GBuffer RT2 worldPos ── Forward
    //     has no GBuffer so safe-no-haze at the host side)
    //
    // When `hazePassEnabled` is false, the DepthHaze pass is not
    // added to the FG, HazeHalf is not declared, and the
    // FrameGraph compile culls it entirely. K3 invariant #2
    // ("hazeEnabled == false ⇒ no FBO allocation") is enforced
    // at compile time, not per-pass. The DepthHazePass::execute
    // path checks `ctx.frameGraph->resolve(HazeHalf)` and early-
    // returns 0 when the resource is not live — byte-equivalent
    // to the pre-F4 `if (!frame.hazeEnabled) return 0` short-
    // circuit.
    const bool hazePassEnabled =
        frame.hazeEnabled
        && frame.hazeStrength > 0.0f
        && (gbufferPassPtr != nullptr)
        && (_impl->viewportW > 0)
        && (_impl->viewportH > 0);
    if (hazePassEnabled) {
        fg.addResource(detail::FgResourceId::HazeHalf,
                       {bgfx::TextureFormat::RGBA8,
                        detail::FgTextureScale::Half,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addPass({"DepthHaze",
                    {detail::FgResourceId::SceneColor},
                    {detail::FgResourceId::HazeHalf},
                    /*enabled=*/true});
    }
    // Compile locks the live set; F6 will add alias decisions on
    // top of this same compile step. F2 only needs the live set
    // so resolve() can return invalid for not-live resources.
    //
    // §F5 (2026-07-24, mid-term FG MVP sub-cut 5) — semantic
    // resolution for the 3 final-PP source slots. Each semantic
    // points to a logical resource; if that resource isn't live
    // (compile culled it because the host disabled the effect)
    // the semantic resolves to invalid ⇒ PostProcessPass binds
    // `sceneColor` on the sampler slot and the FS branchless
    // strength gate collapses the contribution to zero.
    //
    //   FinalColorSource → SceneColor (always; the Base color
    //                       handed to PostProcessPass — Deferred
    //                       ⇒ LightingOutput; Forward ⇒ sceneFbo,
    //                       both routed through the same external
    //                       SceneColor borrow imported above).
    //   BloomSource      → BloomBlurB (when bloomEnabled); else
    //                       invalid ⇒ fallback to sceneColor +
    //                       branchless strength gate (FS sees 0
    //                       bloom contribution).
    //   HazeSource       → HazeHalf (when hazePassEnabled); else
    //                       invalid ⇒ fallback to sceneColor +
    //                       branchless strength gate.
    fg.setResolvedSemantic(detail::FgSemantic::FinalColorSource,
                           detail::FgResourceId::SceneColor);
    if (bloomEnabled) {
        fg.setResolvedSemantic(detail::FgSemantic::BloomSource,
                               detail::FgResourceId::BloomBlurB);
    }
    if (hazePassEnabled) {
        fg.setResolvedSemantic(detail::FgSemantic::HazeSource,
                               detail::FgResourceId::HazeHalf);
    }

    fg.compile();

    detail::PassExecContext ctx{
        _impl->adapter,
        _impl->shaderPool,
        scene,
        _impl->resources.meshes(),
        _impl->resources.textures(),
        _impl->resources.materials(),
        _impl->viewportX,
        _impl->viewportY,
        _impl->viewportW,
        _impl->viewportH,
        frame,
        viewId,
        sceneFbo,
        shadowPassPtr,
        gbufferPassPtr,
        lightingPassPtr,
        // §P5 B7+ (2026-07-22) — host-supplied multi-light DataSource
        // mount. Borrowed pointer from Renderer::setSceneLights.
        // nullptr = B5 single-light path stays active (no behavior
        // change). count == 0 falls through to B5 fallback at
        // LightingPass::execute() side too.
        _impl->sceneLights,
        // §Skybox0 (2026-07-23) — host-supplied Skybox DataSource
        // mount. Borrowed pointer from Renderer::setSkySource.
        // nullptr = no sky mounted (Forward path / SkySource not
        // configured). SkyboxPass early-returns 0; LightingPass
        // binds no gbufferSky sampler — `mix(black, lit, 1) ==
        // lit` collapses to the pre-§Skybox0 dark-frame behavior.
        _impl->skySource,
        // §Skybox0 (2026-07-23) — borrowed pointer to the SkyboxPass
        // instance in the pipeline. nullptr when the Skybox slot is
        // not mounted (Forward default). LightingPass uses this to
        // bind the gbufferSky sampler (mirrors gbufferPassPtr).
        skyboxPassPtr,
        // §P5.5 C (2026-07-23) — borrowed pointer to the host-
        // supplied per-light shadow source. In practice the same
        // SceneLights instance as `sceneLights` above — host
        // populates one SceneLights, renderer reads it via both
        // ptrs (cutsheet reservation pass-lessons-from-deferred
        // .md:330 — "wires per-light shadow via PassExecContext
        // ::perLightShadows borrowed ptr"). nullptr ⇒ ShadowPass
        // pre-C single-key-light fallback + LightingPass
        // perLightShadowCount=0 upload ⇒ byte-equivalent pre-C
        // key-only shadow multiply on lights[0].
        _impl->sceneLights,
        // §S1b BloomBlur (2026-07-23) — borrowed pointer to the
        // BloomExtractPass in the pipeline. BloomBlurPass reads
        // the producer's half-res FBO through this ptr; nullptr
        // ⇒ BloomBlurPass early-returns 0 (no source to blur =
        // visually identical to bloomStrength=0 default).
        bloomExtractPassPtr,
        // §S1c (2026-07-23) — borrowed pointer to the BloomBlurPass
        // in the pipeline. PostProcessPass reads the producer's
        // pongFbo() through this ptr; nullptr ⇒ PostProcessPass
        // binds sceneColor on slot 1 (FS branchless composite
        // collapses to `raw * (1 + 0) = raw` = zero bloom, no
        // GLSL sampler-not-set warning). Mirrors bloomExtractPassPtr
        // above (lifetime contract: pointer must remain valid for
        // the duration of pipeline::executeAll(ctx)).
        bloomBlurPassPtr,
        // §S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — borrowed
        // pointer to the DepthHazePass in the pipeline. PostProcessPass
        // (after §S4c) reads the producer's halfResFbo() through this
        // ptr; nullptr ⇒ PostProcessPass binds sceneColor on slot 2
        // (FS branchless composite collapses to `raw * (1 - 0) = raw`
        // = zero haze, no GLSL sampler-not-set warning). Mirrors
        // bloomBlurPassPtr above (lifetime contract: pointer must
        // remain valid for the duration of pipeline::executeAll(ctx)).
        depthHazePassPtr,
        // §F2 (2026-07-24, mid-term FG MVP) — borrowed pointer to
        // the FrameGraph that owns the post-process chain transient
        // resources (BloomBright / BloomBlurA/B / HazeHalf). F2
        // wires this for BloomExtract only; F3-F5 migrate BloomBlur /
        // DepthHaze / PostProcess. nullptr ⇒ those passes early-
        // return 0 (byte-equivalent to today's host-default path
        // where bloomStrength=0 ⇒ no bloom contribution).
        //
        // The FrameGraph is owned by `_impl->frameGraph`; its
        // lifetime is Renderer lifetime. The pass uses it to resolve
        // a logical FgResourceId (e.g. BloomBright) to a physical
        // bgfx::FrameBufferHandle.
        &_impl->frameGraph,
    };

    static uint32_t s_compositeLog = 0;
    if (s_compositeLog < 3) {
        std::fprintf(stderr,
                     "[ShadowDbg] composite frame=%u viewId=%u viewport=(%u,%u,%u,%u) "
                     "sceneFboValid=%d shadowPass=%p lightDir=(%.2f,%.2f,%.2f)\n",
                     s_compositeLog,
                     static_cast<unsigned>(viewId),
                     static_cast<unsigned>(_impl->viewportX),
                     static_cast<unsigned>(_impl->viewportY),
                     static_cast<unsigned>(_impl->viewportW),
                     static_cast<unsigned>(_impl->viewportH),
                     bgfx::isValid(sceneFbo) ? 1 : 0,
                     static_cast<const void*>(shadowPassPtr),
                     frame.lightDirection.x,
                     frame.lightDirection.y,
                     frame.lightDirection.z);
        ++s_compositeLog;
    }

    // Dispatched via RenderPipeline::executeAll in registration order
    // [ForwardOpaque, Transparent, PostProcess, UI]. ForwardOpaquePass
    // writes the depth buffer first; TransparentPass reuses that depth
    // for STATE_DEPTH_TEST_LESS but does not WRITE_Z so transparent
    // fragments composite over the opaque result without occluding
    // each other (back-to-front sort is via DrawItem::sortKey descending).
    // PostProcessPass samples its own FBO today (scene-RT closure is
    // docs/execution-plan.md P2). UIPass ignores the viewId arg and
    // delegates to its injected UIRenderBackend (see UIPass.h for the
    // chrome lifecycle contract — execute() DOES call flushBatches;
    // beginFrame/endFrame stay on the host's UIManager::render lambda).
    //
    // Per-pass isEnabled() guards are honored by the pipeline. As of
    // E5 (§5.4, 2026-07-22) the canonical default mounts Shadow at
    // slot 0 *enabled* (no opt-in required). Hosts that want to opt
    // out pass a custom desc that omits the Shadow slot — there is no
    // public setShadowsEnabled setter yet (deliberately deferred;
    // Editor / demo / unittest have no consumer). The shadow FBO is
    // a depth-only offscreen target; ShadowPass::execute Noop-gates
    // cleanly when the adapter is uninitialized or Noop.
    _impl->lastDrawCalls = _impl->pipeline.executeAll(ctx);

    // §5.5 cleanup (2026-07-22) — the F1-diagnostic lastFrameShadowFbo
    // cache update is removed. Consumers that need the current shadow
    // FBO call `ctx.shadowPass->shadowFbo()` directly; we no longer
    // mirror it through FrameContext (PR-F1' forbidden combo).

    // §P5 B4c (2026-07-22) — END-OF-FRAME commit prev ← main (NOT
    // beginning-of-render swap — see `Impl::prevMainView` doc-block
    // for why begin-swap would alias prev with the brand-new main
    // and collapse all motion vectors to vec2(0.5, 0.5)).
    //
    // This is the one and only site where the prevMain* cache
    // advances. Next render() reads these as the previous frame's
    // matrices; GBufferPass executes its motion-vector formula
    // against this stored state.
    _impl->prevMainView       = _impl->mainView;
    _impl->prevMainProjection = _impl->mainProjection;
}

void Renderer::resize(uint32_t width, uint32_t height)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }

    // Destroy offscreen RTs before bgfx::reset. Orphaning handles
    // (INVALID without destroy) leaks memory; deferred GBuffer/Lighting
    // also need a forced rebuild (see ensure() allocated-size fix).
    if (detail::BGFXAdapter::isValid(_impl->sceneFbo)) {
        _impl->adapter.destroy(_impl->sceneFbo);
        _impl->sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    _impl->sceneFboW = 0;
    _impl->sceneFboH = 0;
    // Full destroyResources also drops Phoskia programs — fine on
    // rare window resize; MSAA change already does the same.
    if (detail::RenderPass* gbufferPass = _impl->pipeline.findPass("GBuffer")) {
        static_cast<detail::GBufferPass*>(gbufferPass)
            ->destroyResources(_impl->adapter);
    }
    if (detail::RenderPass* lightingPass = _impl->pipeline.findPass("Lighting")) {
        static_cast<detail::LightingPass*>(lightingPass)
            ->destroyResources(_impl->adapter);
    }
    if (detail::RenderPass* shadowPass = _impl->pipeline.findPass("Shadow")) {
        static_cast<detail::ShadowPass*>(shadowPass)
            ->destroyResources(_impl->adapter);
    }
    // §S1a (2026-07-23) — BloomExtractPass destroyResources mirror
    // (mirror Shadow destroy block above). bgfx::reset (triggered
    // by resize) drops view attachments, so the half-res FBO must
    // rebuild on next execute().
    if (detail::RenderPass* bloomExtractPass = _impl->pipeline.findPass("BloomExtract")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::BloomExtractPass*>(bloomExtractPass)
                ->destroyResources(_impl->adapter);
        }
    }
    // §S1b (2026-07-23) — BloomBlurPass destroyResources mirror
    // (mirror BloomExtractPass destroy block above). Both ping-
    // pong FBOs must release before bgfx::reset invalidates the
    // attachments.
    if (detail::RenderPass* bloomBlurPass = _impl->pipeline.findPass("BloomBlur")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::BloomBlurPass*>(bloomBlurPass)
                ->destroyResources(_impl->adapter);
        }
    }
    // §S4b (2026-07-23) — DepthHazePass destroyResources mirror
    // (mirror BloomBlurPass destroy block above). The half-res FBO
    // must release before bgfx::reset invalidates the attachments.
    // When hazeEnabled=false (host default), ensureFbo never ran
    // ⇒ _fbo is invalid ⇒ destroyResources is a no-op (K3
    // invariant #2 holds: zero allocation ⇒ zero release work).
    if (detail::RenderPass* depthHazePass = _impl->pipeline.findPass("DepthHaze")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::DepthHazePass*>(depthHazePass)
                ->destroyResources(_impl->adapter);
        }
    }

    _impl->initDesc.width  = width;
    _impl->initDesc.height = height;
    _impl->adapter.resetResolution(width, height, _impl->initDesc.vsync);
}

bgfx::FrameBufferHandle Renderer::Impl::ensureSceneFbo()
{
    // P2 (PR-D, 2026-07-20) — idempotent scene FBO tracker. Called
    // from render() before PassExecContext is built. Returns the
    // cached handle when size matches; rebuilds when it doesn't or
    // when the previous build returned invalid.
    if (!adapter.isInitialized()) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    const uint32_t w = viewportW;
    const uint32_t h = viewportH;
    if (w == 0 || h == 0) {
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (detail::BGFXAdapter::isValid(sceneFbo) && sceneFboW == w && sceneFboH == h) {
        return sceneFbo;
    }
    if (detail::BGFXAdapter::isValid(sceneFbo)) {
        adapter.destroy(sceneFbo);
        sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    sceneFbo = adapter.createColorDepthFrameBuffer(static_cast<uint16_t>(w),
                                                   static_cast<uint16_t>(h));
    if (detail::BGFXAdapter::isValid(sceneFbo)) {
        sceneFboW = static_cast<uint16_t>(w);
        sceneFboH = static_cast<uint16_t>(h);
    } else {
        sceneFboW = 0;
        sceneFboH = 0;
        std::fprintf(stderr,
                     "[Renderer] scene FBO create failed at %ux%u; "
                     "ForwardOpaque/Transparent will draw to the backbuffer this frame\n",
                     w, h);
    }
    return sceneFbo;
}

void Renderer::setViewportRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    if (!_impl) {
        return;
    }
    _impl->viewportX = x;
    _impl->viewportY = y;
    _impl->viewportW = width;
    _impl->viewportH = height;
}

void Renderer::endFrame()
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }

    if (!_impl->pendingScreenshotBase.empty()) {
        _impl->adapter.requestScreenshot(_impl->pendingScreenshotBase);
        _impl->finalizeScreenshotBase = _impl->pendingScreenshotBase;
        _impl->pendingScreenshotBase.clear();
    }

    _impl->debugOverlay.onEndFrame(_impl->lastDrawCalls, _impl->lastSceneItems,
                                   _impl->viewportX, _impl->viewportY,
                                   _impl->viewportW, _impl->viewportH);
    _impl->adapter.endFrame();
    _impl->compositeSceneViewId = -1;

    if (!_impl->finalizeScreenshotBase.empty()) {
        detail::finalizeScreenshotSidecar(_impl->finalizeScreenshotBase);
        _impl->finalizeScreenshotBase.clear();
    }
}

MeshHandle Renderer::createMesh(const void* vertices,
                                uint32_t vertexCount,
                                const VertexLayoutDesc& layout,
                                const uint16_t* indices,
                                uint32_t indexCount)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createMesh(vertices, vertexCount, layout, indices, indexCount);
}

MeshHandle Renderer::createMesh32(const void* vertices,
                                  uint32_t vertexCount,
                                  const VertexLayoutDesc& layout,
                                  const uint32_t* indices,
                                  uint32_t indexCount)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createMesh32(vertices, vertexCount, layout, indices, indexCount);
}

MeshHandle Renderer::loadMesh(const std::string& path)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.loadMesh(path);
}

MeshHandle Renderer::createUnitCube()
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createUnitCube();
}

MeshHandle Renderer::createTexturedUnitCube()
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createTexturedUnitCube();
}

MaterialHandle Renderer::createMaterialFromPhoskia(const std::string& source,
                                                   const std::string& cacheKey)
{
    if (!_impl || !_impl->shaderPoolReady) {
        return {};
    }
    return _impl->resources.createMaterialFromPhoskia(source, cacheKey);
}

MaterialHandle Renderer::createMaterialFromBgfxSc(const std::string& vertexSc,
                                                  const std::string& fragmentSc,
                                                  const std::string& varyingDefSc,
                                                  const std::string& cacheKey)
{
    if (!_impl || !_impl->shaderPoolReady) {
        return {};
    }
    return _impl->resources.createMaterialFromBgfxSc(vertexSc, fragmentSc, varyingDefSc,
                                                    cacheKey);
}

MaterialHandle Renderer::createMaterialFromFile(const std::string& path)
{
    if (!_impl || !_impl->shaderPoolReady) {
        return {};
    }
    return _impl->resources.createMaterialFromFile(path);
}

MaterialHandle Renderer::loadMaterial(const std::string& path)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        std::fprintf(stderr, "[Renderer] loadMaterial: renderer not initialized\n");
        return {};
    }
    if (!_impl->shaderPoolReady) {
        std::fprintf(stderr,
                     "[Renderer] loadMaterial: shader pool not ready (check shaderc path)\n");
        return {};
    }
    return _impl->resources.loadMaterial(path);
}

TextureHandle Renderer::createTextureFromRgba8(uint32_t width, uint32_t height,
                                               const uint8_t* pixels,
                                               const std::string& cacheKey)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createTextureFromRgba8(width, height, pixels, cacheKey);
}

TextureHandle Renderer::createCubeTextureFromRgba8(uint32_t size,
                                                   const uint8_t* rgba8Faces,
                                                   const std::string& cacheKey)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createCubeTextureFromRgba8(size, rgba8Faces, cacheKey);
}

TextureHandle Renderer::createTextureFromFile(const std::string& path,
                                              const std::string& cacheKey)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createTextureFromFile(path, cacheKey);
}

TextureHandle Renderer::loadTexture(const std::string& path)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.loadTexture(path);
}

void Renderer::setMaterialColor(MaterialHandle material, const char* propertyName,
                                float r, float g, float b, float a)
{
    if (!_impl) {
        return;
    }
    _impl->resources.setMaterialColor(material, propertyName, r, g, b, a);
}

void Renderer::setMaterialBlendMode(MaterialHandle material, BlendMode blendMode)
{
    // Inline mutates RenderResourceManager's _materials map directly —
    // the GpuMaterial field is a single POD byte (BlendMode uint8_t)
    // and threading the setter through RenderResourceManager for one
    // byte is not worth the surface. Public API no-throws on bad
    // handle, mirroring setMaterialColor above.
    if (!_impl) {
        return;
    }
    auto& mats = _impl->resources.materials();
    auto it = mats.find(material.id);
    if (it == mats.end()) {
        return;
    }
    it->second.blendMode = blendMode;
}

void Renderer::setMaterialFloat(MaterialHandle material, const char* uniformName, float value)
{
    if (!_impl) {
        return;
    }
    _impl->resources.setMaterialFloat(material, uniformName, value);
}

void Renderer::setMaterialVec3(MaterialHandle material, const char* uniformName,
                               float x, float y, float z)
{
    if (!_impl) {
        return;
    }
    _impl->resources.setMaterialVec3(material, uniformName, x, y, z);
}

void Renderer::setMaterialMatrix4(MaterialHandle material, const char* uniformName,
                                  const ayt::math::Float4x4& matrix)
{
    if (!_impl) {
        return;
    }
    _impl->resources.setMaterialMatrix4(material, uniformName, matrix);
}

void Renderer::setMaterialTexture(MaterialHandle material, const char* textureBindingName,
                                  TextureHandle texture)
{
    if (!_impl) {
        return;
    }
    _impl->resources.setMaterialTexture(material, textureBindingName, texture);
}

void Renderer::setMainCamera(const ayt::math::Float4x4& view,
                             const ayt::math::Float4x4& projection)
{
    if (!_impl) {
        return;
    }
    _impl->mainView       = view;
    _impl->mainProjection = projection;
}

void Renderer::setDirectionalLight(const ayt::math::FVector3& direction,
                                   const ayt::math::FVector3& color)
{
    if (!_impl) {
        return;
    }
    _impl->directionalLightDir   = direction.normalize();
    _impl->directionalLightColor = color;
}

// §P5 B7+ (2026-07-22) — host-facing multi-light DataSource mount.
// Borrowed pointer pattern; lifetime contract: the SceneLights
// must outlive render(). Default nullptr = single-light path
// (FrameContext::lightDirection / lightColor as set above by
// setDirectionalLight).
//
// Passing a SceneLights with count == 0 has the same effect as
// nullptr (LightingPass iterates 0 lights and the B5 fallback
// kicks in via the existence check at execute() time).
void Renderer::setSceneLights(const ayt::render::SceneLights* lights)
{
    if (!_impl) {
        return;
    }
    _impl->sceneLights = lights;
}

// §Skybox0 (2026-07-23) — public setter for the host-supplied
// Skybox DataSource. Borrowed pointer pattern (mirror
// setSceneLights above): lifetime is the host's responsibility,
// the renderer reads the pointer each frame via
// `PassExecContext::skySource` without copying. nullptr / inactive
// SkySource (`!isActive()`) ⇒ SkyboxPass early-returns 0;
// LightingPass binds no gbufferSky sampler; pre-§Skybox0 dark-frame
// behavior preserved on Forward / no-sky hosts.
void Renderer::setSkySource(const ayt::render::SkySource* sky)
{
    if (!_impl) {
        return;
    }
    _impl->skySource = sky;
}

const ayt::render::SkySource* Renderer::skySource() const noexcept
{
    if (!_impl) {
        return nullptr;
    }
    return _impl->skySource;
}

// §P5.5 D (2026-07-23) — IBL MVP (Ambient Diffuse Cube Lookup).
// Host-side cube map handle upload. Mirror setSkySource() shape
// (Impl member write + nullptr/early-return guard) but the
// payload is a TextureHandle resource, not a borrowed SkySource
// pointer. Clear-by-invalid semantics: pass TextureHandle{} to
// revert to the equirect path.
//
// Forwarding: the cube handle is also pushed into the
// SkyboxPass producer state via `findPass("Skybox")→setCubeTexture`
// so SkyboxPass::execute and LightingPass::execute can both
// read it (LightingPass via `ctx.skyboxPass->cubeTexture()`) on
// the live pipeline. When the Skybox slot isn't mounted
// (Forward makeDefault()), the cube handle is cached here but
// has no observable effect — Forward hosts see 0 behavior change
// per cutsheet §5.3 red line #4.
//
// The hard rule (cube valid ⇒ CubeMap path; otherwise equirect)
// is enforced downstream by SkyboxPass::execute +
// LightingPass::execute reading the cube handle + SkySource::kind
// together.
void Renderer::setSkySourceCube(ayt::render::TextureHandle cube)
{
    if (!_impl) {
        return;
    }
    _impl->skyCubeTexture = cube;
    // Forward to the SkyboxPass producer (cutsheet producer-state
    // pattern — mirror shadowPass / lightingPass borrowed-ptr
    // shape, but the cube handle is a Resource not a borrowed
    // pointer). No-op when Skybox slot isn't mounted.
    if (detail::RenderPass* skyboxPass = _impl->pipeline.findPass("Skybox")) {
        static_cast<detail::SkyboxPass*>(skyboxPass)
            ->setCubeTexture(cube);
    }
}

ayt::render::TextureHandle Renderer::skySourceCube() const noexcept
{
    return _impl ? _impl->skyCubeTexture : ayt::render::TextureHandle{};
}

void Renderer::setAmbientStrength(float strength)
{
    if (!_impl) {
        return;
    }
    _impl->ambientStrength = strength;
    // Eager push when Lighting is mounted; render() also broadcasts
    // so configurePipeline recreation cannot drop the host knob.
    if (detail::RenderPass* lightingSlot = _impl->pipeline.findPass("Lighting")) {
        static_cast<detail::LightingPass*>(lightingSlot)
            ->setAmbientStrength(strength);
    }
}

float Renderer::ambientStrength() const noexcept
{
    return _impl ? _impl->ambientStrength : 0.6f;
}

void Renderer::setMsaaSampleCount(uint32_t samples)
{
    if (!_impl) {
        return;
    }
    const uint32_t before = _impl->adapter.msaaSampleCount();
    _impl->adapter.setMsaaSampleCount(samples);
    _impl->initDesc.msaa = _impl->adapter.msaaSampleCount();
    // bgfx::reset drops view attachments; recycle scene RT like resize().
    if (before != _impl->initDesc.msaa) {
        if (detail::BGFXAdapter::isValid(_impl->sceneFbo)) {
            _impl->adapter.destroy(_impl->sceneFbo);
            _impl->sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        }
        _impl->sceneFboW = 0;
        _impl->sceneFboH = 0;
        if (detail::RenderPass* shadowPass = _impl->pipeline.findPass("Shadow")) {
            if (_impl->adapter.isInitialized()) {
                static_cast<detail::ShadowPass*>(shadowPass)
                    ->destroyResources(_impl->adapter);
            }
        }
        // §P5 B4a (2026-07-22) — GBuffer destroyResources mirror.
        // `bgfx::reset` (triggered by msaa change at the `before != new`
        // branch) drops view attachments, so the GBuffer FBO must
        // rebuild on next execute(). Matches Shadow destroy mirror
        // above — Shadow re-ensures via _mapResources.ensure(); GBuffer
        // re-ensures via this class's `ensure()` in execute().
        if (detail::RenderPass* gbufferPass = _impl->pipeline.findPass("GBuffer")) {
            if (_impl->adapter.isInitialized()) {
                static_cast<detail::GBufferPass*>(gbufferPass)
                    ->destroyResources(_impl->adapter);
            }
        }
        // §P5 B5 (2026-07-22) — LightingPass destroyResources mirror.
        // LightingPass owns LightingOutput FBO + fullscreen triangle
        // VB/IB + Phoskia Lighting program; all three must be released
        // before bgfx::reset invalidates the attachments. Same handle-
        // rotation reasoning as GBuffer.
        if (detail::RenderPass* lightingPass = _impl->pipeline.findPass("Lighting")) {
            if (_impl->adapter.isInitialized()) {
                static_cast<detail::LightingPass*>(lightingPass)
                    ->destroyResources(_impl->adapter);
            }
        }
        // §S1a (2026-07-23) — BloomExtractPass destroyResources
        // mirror. bgfx::reset (triggered by MSAA change at the
        // `before != new` branch) drops view attachments, so the
        // half-res FBO must rebuild on next execute().
        if (detail::RenderPass* bloomExtractPass = _impl->pipeline.findPass("BloomExtract")) {
            if (_impl->adapter.isInitialized()) {
                static_cast<detail::BloomExtractPass*>(bloomExtractPass)
                    ->destroyResources(_impl->adapter);
            }
        }
        // §S1b (2026-07-23) — BloomBlurPass destroyResources
        // mirror. Both ping-pong FBOs must release before
        // bgfx::reset invalidates the attachments.
        if (detail::RenderPass* bloomBlurPass = _impl->pipeline.findPass("BloomBlur")) {
            if (_impl->adapter.isInitialized()) {
                static_cast<detail::BloomBlurPass*>(bloomBlurPass)
                    ->destroyResources(_impl->adapter);
            }
        }
    }
}

uint32_t Renderer::msaaSampleCount() const noexcept
{
    return _impl ? _impl->adapter.msaaSampleCount() : 0u;
}

void Renderer::setShadowPcfEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    _impl->shadowPcfEnabled = enabled;
    _impl->initDesc.shadowPcf = enabled;
    _impl->applyShadowQualityKnobs();
}

bool Renderer::shadowPcfEnabled() const noexcept
{
    return _impl && _impl->shadowPcfEnabled;
}

void Renderer::setShadowBias(float bias)
{
    // P4.2 (§P4, 2026-07-22) — global shadow receiver bias knob.
    // Range guidance: 0 (disable) to 0.01 (very strong; expect
    // peter-panning). Negative values are accepted for completeness
    // but produce "shadows behind the surface" artifacts in most
    // Phoskia receivers — host responsibility. No clamping here;
    // matches setMaterialFloat / setMaterialVec3 leniency.
    if (!_impl) {
        return;
    }
    _impl->shadowBias = bias;
}

float Renderer::shadowBias() const noexcept
{
    return _impl ? _impl->shadowBias : 0.003f;
}

bool Renderer::shadowsEnabled() const noexcept
{
    // E5 (§5.4, 2026-07-22) — live read of the Shadow slot's enabled
    // flag. Mirrors shadowPcfEnabled() but reads the pipeline directly
    // (no Impl mirror) because the flag is owned by the pass itself.
    // When no Shadow slot is mounted (e.g. host passed a desc without
    // it), returns false. Public surface const-noexcept; safe to call
    // from any host observer.
    if (!_impl) {
        return false;
    }
    const detail::RenderPass* shadow = _impl->pipeline.findPass("Shadow");
    return shadow != nullptr && shadow->isEnabled();
}

bool Renderer::lightingEnabled() const noexcept
{
    // §P5 B3 (2026-07-22) — live read of the Lighting slot's enabled
    // flag. Mirrors shadowsEnabled() (E5 pattern). When no Lighting
    // slot is mounted (e.g. host on Forward pipeline or passes a
    // custom desc without it), returns false. Public surface
    // const-noexcept; safe to call from any host observer.
    if (!_impl) {
        return false;
    }
    const detail::RenderPass* lighting = _impl->pipeline.findPass("Lighting");
    return lighting != nullptr && lighting->isEnabled();
}

void Renderer::setPostProcessBloomStrength(float strength)
{
    if (!_impl) {
        return;
    }
    // R5+ — clamps negative values; values >1 are accepted (the shader
    // is responsible for clamping the final mix). NaN/Inf pass through
    // and the shader sees them — matches the existing setMaterialFloat
    // leniency (no validation, host responsibility).
    _impl->postProcessBloomStrength = strength;
}

void Renderer::setPostProcessExposure(float exposure)
{
    if (!_impl) {
        return;
    }
    _impl->postProcessExposure = exposure;
}

void Renderer::setPostProcessGamma(float gamma)
{
    if (!_impl) {
        return;
    }
    _impl->postProcessGamma = gamma;
}

void Renderer::setPostProcessClockPaused(bool paused)
{
    if (!_impl) {
        return;
    }
    if (paused && !_impl->renderClockPaused
        && _impl->renderClockOrigin.time_since_epoch().count() != 0) {
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = now - _impl->renderClockOrigin;
        _impl->renderClockFrozenSeconds = elapsed.count();
    }
    _impl->renderClockPaused = paused;
}

bool Renderer::isPostProcessClockPaused() const noexcept
{
    return _impl != nullptr && _impl->renderClockPaused;
}

void Renderer::setSimulationTimeSeconds(float seconds)
{
    if (!_impl) {
        return;
    }
    _impl->hasSimulationTime = true;
    _impl->simulationTimeSeconds = seconds;
}

void Renderer::setPostProcessTonemapMode(TonemapMode mode)
{
    if (!_impl) {
        return;
    }
    // Bridge from public AYRenderer::TonemapMode to detail::FrameContext
    // enum. Both share the same underlying values (0/1/2) by design
    // (see AYRenderer.h:post-process setter block + FrameContext.h),
    // but going through the cast keeps the two enums structurally
    // independent so a future change to FrameContext::TonemapMode
    // ordering doesn't silently break the public surface.
    switch (mode) {
    case TonemapMode::None:
        _impl->postProcessTonemapMode = detail::FrameContext::TonemapMode::None;
        break;
    case TonemapMode::Reinhard:
        _impl->postProcessTonemapMode = detail::FrameContext::TonemapMode::Reinhard;
        break;
    case TonemapMode::ACES:
        _impl->postProcessTonemapMode = detail::FrameContext::TonemapMode::ACES;
        break;
    }
}

void Renderer::setDepthHazeEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    _impl->depthHazeEnabled = enabled;
}

bool Renderer::depthHazeEnabled() const noexcept
{
    return _impl != nullptr && _impl->depthHazeEnabled;
}

void Renderer::setDepthHazeStrength(float strength)
{
    if (!_impl) {
        return;
    }
    _impl->depthHazeStrength = strength;
}

float Renderer::depthHazeStrength() const noexcept
{
    return _impl ? _impl->depthHazeStrength : 0.0f;
}

void Renderer::setDepthHazeDensity(float density)
{
    if (!_impl) {
        return;
    }
    _impl->depthHazeDensity = density;
}

float Renderer::depthHazeDensity() const noexcept
{
    return _impl ? _impl->depthHazeDensity : 0.02f;
}

void Renderer::setDepthHazeColor(const ayt::math::FVector3& color)
{
    if (!_impl) {
        return;
    }
    _impl->depthHazeColor = color;
}

ayt::math::FVector3 Renderer::depthHazeColor() const noexcept
{
    return _impl ? _impl->depthHazeColor
                 : ayt::math::FVector3(0.55f, 0.65f, 0.78f);
}

void Renderer::setDepthHazeParams(float density, const ayt::math::FVector3& fogColor)
{
    setDepthHazeDensity(density);
    setDepthHazeColor(fogColor);
}

void Renderer::configurePipeline(const RenderPipelineDesc& desc)
{
    if (!_impl) {
        return;
    }
    _impl->applyPipelineDesc(desc);
}

const RenderPipelineDesc& Renderer::pipelineDesc() const noexcept
{
    static const RenderPipelineDesc kEmpty = RenderPipelineDesc::makeDefault();
    if (!_impl) {
        return kEmpty;
    }
    return _impl->pipelineDesc;
}

void Renderer::setMainCameraLookAtPerspective(const ayt::math::FVector3& eye,
                                              const ayt::math::FVector3& at,
                                              const ayt::math::FVector3& up,
                                              float fovYDegrees,
                                              float aspect,
                                              float nearZ,
                                              float farZ)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }

    // bx for lookAt/proj (homogeneousDepth for D3D). Store as true AYMath
    // via fromBgfxColumnMajor — never memcpy column-major bytes into Float4x4.
    float viewBx[16];
    float projBx[16];
    const bx::Vec3 eyeBx = {eye.x, eye.y, eye.z};
    const bx::Vec3 atBx  = {at.x, at.y, at.z};
    const bx::Vec3 upBx  = {up.x, up.y, up.z};
    bx::mtxLookAt(viewBx, eyeBx, atBx, upBx);
    bx::mtxProj(projBx, fovYDegrees, aspect, nearZ, farZ,
                bgfx::getCaps()->homogeneousDepth);

    _impl->mainCameraPosition = eye;
    setMainCamera(detail::fromBgfxColumnMajor(viewBx),
                  detail::fromBgfxColumnMajor(projBx));
}

ayt::math::FVector3 Renderer::mainCameraPosition() const noexcept
{
    return _impl ? _impl->mainCameraPosition : ayt::math::FVector3(0.0f, 0.0f, 4.0f);
}

void Renderer::destroyMesh(MeshHandle& mesh)
{
    if (!_impl) {
        mesh = {};
        return;
    }
    _impl->resources.destroyMesh(mesh);
}

void Renderer::destroyMaterial(MaterialHandle& material)
{
    if (!_impl) {
        material = {};
        return;
    }
    _impl->resources.destroyMaterial(material);
}

void Renderer::destroyTexture(TextureHandle& texture)
{
    if (!_impl) {
        texture = {};
        return;
    }
    _impl->resources.destroyTexture(texture);
}

void Renderer::pollShaderHotReload()
{
    if (_impl && _impl->shaderPoolReady) {
        _impl->shaderPool.pollHotReload();
        _impl->resources.refreshMaterialsAfterHotReload();
    }
}

uint32_t Renderer::reloadMaterialsForShaderFile(const std::string& shaderPath)
{
    if (!_impl || !_impl->shaderPoolReady || shaderPath.empty()) {
        return 0;
    }
    return _impl->resources.reloadMaterialsForShaderFile(shaderPath);
}

void Renderer::setDebugOverlayEnabled(bool enabled)
{
    if (_impl) {
        _impl->debugOverlay.setEnabled(enabled);
    }
}

bool Renderer::isDebugOverlayEnabled() const noexcept
{
    return _impl && _impl->debugOverlay.isEnabled();
}

void Renderer::setDebugOverlaySuppressed(bool suppressed)
{
    if (_impl) {
        _impl->debugOverlay.setSuppressed(suppressed);
    }
}

bool Renderer::isDebugOverlaySuppressed() const noexcept
{
    return _impl && _impl->debugOverlay.isSuppressed();
}

void Renderer::resetDebugOverlayStats()
{
    if (_impl) {
        _impl->debugOverlay.resetStats();
    }
}

const RenderFrameStats& Renderer::getFrameStats() const noexcept
{
    static const RenderFrameStats kEmpty{};
    return _impl ? _impl->debugOverlay.stats() : kEmpty;
}

bool Renderer::captureScreenshot(const std::string& filePath)
{
    if (!_impl || !_impl->adapter.isInitialized() || filePath.empty()) {
        return false;
    }
    if (_impl->initDesc.backend == Backend::Noop || _impl->initDesc.windowHandle == nullptr) {
        return false;
    }
    _impl->pendingScreenshotBase = detail::screenshotBasePath(filePath);
    return true;
}

size_t Renderer::meshCacheSize() const
{
    if (!_impl) return 0;
    return _impl->resources.meshCacheSize();
}

size_t Renderer::materialCacheSize() const
{
    if (!_impl) return 0;
    return _impl->resources.materialCacheSize();
}

void Renderer::setShaderIntermediateDumpDirectory(const std::string& dir)
{
    if (!_impl || !_impl->shaderPoolReady || dir.empty()) {
        return;
    }
    _impl->shaderPool.setIntermediateDumpDirectory(dir);
}

void Renderer::setShaderCacheDirectory(const std::string& dir)
{
    if (!_impl || !_impl->shaderPoolReady || dir.empty()) {
        return;
    }
    _impl->shaderPool.setCacheDirectory(dir);
}

detail::BGFXAdapter* Renderer::bgfxAdapter() noexcept
{
    return _impl ? &_impl->adapter : nullptr;
}

const detail::BGFXAdapter* Renderer::bgfxAdapter() const noexcept
{
    return _impl ? &_impl->adapter : nullptr;
}

ayt::shader::ShaderResourcePool* Renderer::shaderPool() noexcept
{
    return _impl && _impl->shaderPoolReady ? &_impl->shaderPool : nullptr;
}

const ayt::shader::ShaderResourcePool* Renderer::shaderPool() const noexcept
{
    return _impl && _impl->shaderPoolReady ? &_impl->shaderPool : nullptr;
}

bool Renderer::initializeUiRenderBackend(UIRenderBackend& backend)
{
    // DEPRECATED — U1+. New hosts should call setUiBackend directly.
    // Retained for backward compat: this API used to be the ONLY way
    // to hand the backend its private initializeFromRenderer pointer
    // (called transitively via UIRenderBackend::initialize(renderer)).
    // Today most hosts call UIRenderBackend::initialize(renderer)
    // directly and use setUiBackend to inject — see AYEditorApp.cpp:452
    // and ShutdownRepro.cpp:233. This wrapper preserves both legacy
    // paths: GPU init via initializeFromRenderer AND pointer injection
    // into the pipeline's UIPass.
    detail::BGFXAdapter* adapter = bgfxAdapter();
    ayt::shader::ShaderResourcePool* pool = shaderPool();
    if (adapter == nullptr || pool == nullptr || !adapter->isInitialized()) {
        return false;
    }
    const bool ok = backend.initializeFromRenderer(*this, *adapter, *pool);
    setUiBackend(&backend);
    return ok;
}

void Renderer::shutdownUiRenderBackend(UIRenderBackend& backend)
{
    detail::BGFXAdapter* adapter = bgfxAdapter();
    ayt::shader::ShaderResourcePool* pool = shaderPool();
    if (adapter != nullptr && adapter->isInitialized() && pool != nullptr) {
        backend.shutdownFromRenderer(*adapter, *pool);
    } else {
        backend.shutdownFromRendererWithoutAdapter();
    }
}

void Renderer::setUiBackend(UIRenderBackend* backend)
{
    // U1+ — locate the UI pass by name() to keep Impl ignorant of the
    // concrete UIPass type (U0's polymorphism contract). The lookup is
    // O(N) over pipeline.passes() (3–7 entries max) and only fires at
    // host init, not per-frame, so the cost is negligible. If a future
    // pass also returns name() == "UI" this will hand it the backend
    // pointer too — that would be a configuration bug, not an API bug.
    if (!_impl) {
        return;
    }
    if (detail::RenderPass* uiPass = _impl->pipeline.findPass("UI")) {
        static_cast<detail::UIPass*>(uiPass)->setBackend(backend);
    }
}

} // namespace ayt::render
