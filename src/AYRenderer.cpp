#include "AYRenderer.h"

#include "AYRenderer/F1DiagFlags.h"
#include "detail/BGFXAdapter.h"
#include "detail/BgfxMatrix.h"
#include "detail/CameraMath.h"
#include "detail/ColorGradingPass.h"
#include "detail/BloomExtractPass.h"
#include "detail/BloomBlurPass.h"
#include "detail/BloomPipeline.h"
#include "detail/DepthHazePass.h"  // S4b (2026-07-23) — borrowed-ptr source for PassExecContext::depthHazePass + destroyResources.
#include "detail/DepthHazePipeline.h"
#include "detail/DebugOverlay.h"
#include "detail/FgResource.h"        // §F2 (2026-07-24) — FrameGraph FgResourceId + FgTextureDesc
#include "detail/ForwardOpaquePass.h"
#include "detail/FXAAPass.h"
#include "detail/SMAAPass.h"
#include "detail/GBufferDebugPass.h"
#include "detail/GBufferPass.h"
#include "detail/FrameContext.h"
#include "detail/LightingPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/PostProcessPipeline.h"
#include "detail/PresentPass.h"
#include "detail/EditorOverlayPass.h"
#include "detail/Forward2DOpaquePass.h"  // CM-1 (2026-08-11) — 2D lane pass factory.
#include "detail/RenderPipeline.h"
#include "detail/RenderTargetPool.h"
#include "detail/RenderViewOrder.h"
#include "detail/SSAOPass.h"        // §A1 SSAO MVP (2026-07-24) — SSAOPass factory + FrameGraph SSAOTexture resolve gate.
#include "detail/SSAOPipeline.h"
#include "detail/RenderResourceManager.h"
#include "detail/ScreenshotSidecar.h"
#include "detail/ShaderPoolSetup.h"
#include "detail/ShadowPass.h"
#include "detail/SkyboxPass.h"
#include "detail/TransparentPass.h"
#include "detail/UiGpuContext.h"
#include "detail/UIPass.h"
#include "AYRenderer/UIRenderBackend.h"

#include "AYMath/CoordinateConvention.h"

#include "AYShader/ShaderResourcePool.h"
#include "AYResource/ResourceManager.h"

#include <bgfx/bgfx.h>

#include <AYIO/Env.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
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
        // CM-1 (2026-08-11) — 2D lane between ForwardOpaque and
        // Transparent. Zero-cost when the scene carries no
        // DrawPayload2D items (pass returns 0 draws) ⇒ pre-CM-1
        // Forward hosts see 0 behavior change.
        RenderPassSlot::Forward2DOpaque,
        // Forward has no GBuffer, so DepthHaze remains a zero-cost no-op. Its
        // position matches the deferred dependency order for custom pipelines.
        RenderPassSlot::DepthHaze,
        RenderPassSlot::Transparent,
        RenderPassSlot::BloomExtract,   // S1a (2026-07-23) — half-res bright extract; bloomStrength=0 default ⇒ zero write.
        RenderPassSlot::BloomBlur,      // S1b (2026-07-23) — half-res separable-Gaussian blur ping-pong; bloomStrength=0 default ⇒ zero write.
        RenderPassSlot::PostProcess,
        RenderPassSlot::FXAA,
        RenderPassSlot::SMAA,
        RenderPassSlot::ColorGrading,
        RenderPassSlot::Present,
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

namespace {

void ensurePresentAfterPostProcess(RenderPipelineDesc& desc)
{
    auto ppIt = std::find(desc.passes.begin(), desc.passes.end(),
                          RenderPassSlot::PostProcess);
    const auto presentIt = std::find(desc.passes.begin(), desc.passes.end(),
                                     RenderPassSlot::Present);
    if (ppIt != desc.passes.end() && presentIt == desc.passes.end()) {
        auto insertAfter = ppIt;
        const auto fxaaIt = std::find(desc.passes.begin(), desc.passes.end(),
                                      RenderPassSlot::FXAA);
        if (fxaaIt != desc.passes.end() && fxaaIt > insertAfter) {
            insertAfter = fxaaIt;
        }
        const auto gradingIt = std::find(desc.passes.begin(), desc.passes.end(),
                                         RenderPassSlot::ColorGrading);
        if (gradingIt != desc.passes.end() && gradingIt > insertAfter) {
            insertAfter = gradingIt;
        }
        const auto smaaIt = std::find(desc.passes.begin(), desc.passes.end(),
                                      RenderPassSlot::SMAA);
        if (smaaIt != desc.passes.end() && smaaIt > insertAfter) {
            insertAfter = smaaIt;
        }
        desc.passes.insert(insertAfter + 1, RenderPassSlot::Present);
    }
}

void insertEditorOverlayAfterPresent(RenderPipelineDesc& desc)
{
    auto presentIt = std::find(desc.passes.begin(), desc.passes.end(),
                               RenderPassSlot::Present);
    if (presentIt != desc.passes.end()) {
        desc.passes.insert(presentIt + 1, RenderPassSlot::EditorOverlay);
    } else {
        desc.passes.push_back(RenderPassSlot::EditorOverlay);
    }
}

} // namespace

RenderPipelineDesc RenderPipelineDesc::makeEditorForward()
{
    RenderPipelineDesc desc = makeForwardWithShadows();
    insertEditorOverlayAfterPresent(desc);
    return desc;
}

RenderPipelineDesc RenderPipelineDesc::makeEditorDeferred()
{
    RenderPipelineDesc desc = makeDeferred();
    insertEditorOverlayAfterPresent(desc);
    return desc;
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
        RenderPassSlot::SSAO,
        RenderPassSlot::Lighting,
        RenderPassSlot::DepthHaze,
        RenderPassSlot::Transparent,
        RenderPassSlot::BloomExtract,   // S1a (2026-07-23) — half-res bright extract; bloomStrength=0 default ⇒ zero write.
        RenderPassSlot::BloomBlur,      // S1b (2026-07-23) — half-res separable-Gaussian blur ping-pong; bloomStrength=0 default ⇒ zero write.
        RenderPassSlot::PostProcess,
        RenderPassSlot::FXAA,
        RenderPassSlot::SMAA,
        RenderPassSlot::ColorGrading,
        RenderPassSlot::Present,
        RenderPassSlot::UI,
        // Fullscreen attachment overlay on view 250. It is mounted only in
        // Deferred, defaults off, and executes before editor UI (view 255).
        RenderPassSlot::GBufferDebug,
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
    case RenderPassSlot::FXAA:
        return std::make_unique<detail::FXAAPass>();
    case RenderPassSlot::SMAA:
        return std::make_unique<detail::SMAAPass>();
    case RenderPassSlot::ColorGrading:
        return std::make_unique<detail::ColorGradingPass>();
    case RenderPassSlot::Present:
        return std::make_unique<detail::PresentPass>();
    case RenderPassSlot::EditorOverlay:
        return std::make_unique<detail::EditorOverlayPass>();
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
    // §A1 SSAO MVP (2026-07-24) — SSAO pass factory. Only mounted
    // when the pipeline desc includes RenderPassSlot::SSAO (i.e.
    // a Deferred desc or a custom desc that opts in). makeDefault()
    // does NOT include this slot ⇒ Forward hosts see 0 behavior
    // change (cutsheet §S2 hard line). render() central
    // `ssaoPassEnabled` further gates the FG resource add/addPass
    // — when ssaoEnabled=false (default), the FG compile culls
    // SSAOTexture and SSAOPass::execute returns 0.
    case RenderPassSlot::SSAO:
        return std::make_unique<detail::SSAOPass>();
    // Deferred-only GBuffer attachment overlay. Default-off execution gates
    // before allocating its fullscreen triangle or shader program.
    case RenderPassSlot::GBufferDebug:
        return std::make_unique<detail::GBufferDebugPass>();
    // CM-1 (2026-08-11) — 2D lane. Mounted only when the desc
    // includes RenderPassSlot::Forward2DOpaque (makeDefault() does;
    // makeDeferred() does NOT — 2D is Forward-path-only per the
    // slot comment in AYRenderer/RenderTypes.h). Zero payload items ⇒
    // execute() returns 0 (zero behavior change for 3D hosts).
    case RenderPassSlot::Forward2DOpaque:
        return std::make_unique<detail::Forward2DOpaquePass>();
    }
    return nullptr;
}

} // namespace

struct Renderer::Impl {
    detail::BGFXAdapter           adapter;
    ayt::shader::ShaderResourcePool    shaderPool;
    detail::RenderTargetPool      renderTargetPool{adapter};
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

    // FrameGraph owns the post-process chain transient resources:
    // half-resolution BloomBright/BloomBlurA/B, full-resolution HazeColor,
    // and full-resolution SSAOTexture. It lives on Impl so its lifetime
    // matches Renderer and is borrowed through PassExecContext each frame.
    // Constructed with the adapter reference (it queries the
    // adapter's isInitialized/isNoopBackend per-frame).
    detail::FrameGraph            frameGraph{adapter, renderTargetPool};

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
    float                          postProcessBloomThreshold = 1.0f;
    float                          postProcessBloomSoftKnee  = 0.5f;
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

    // §S2 v1 — SSAO host knobs (FrameContext defaults stay off).
    bool                           ssaoEnabled  = false;
    float                          ssaoStrength = 0.0f;
    float                          ssaoRadius   = 0.5f;
    float                          ssaoBias     = 0.025f;

    // GBuffer attachment overlay knobs. Channel 3 is the packed material
    // scalar view; the public byte setter keeps the host surface lightweight.
    bool                           gbufferDebugEnabled  = false;
    uint8_t                        gbufferDebugChannel  = 0;

    // EditorOverlay orientation widget. Stored on Impl so pipeline rebuilds
    // preserve the host's toggle even though the pass object is recreated.
    bool                           viewportOrientationAxisEnabled = false;
    EditorTransformGizmoState      editorTransformGizmo{};

    // Display-referred anti-aliasing is enabled by default. The state lives on
    // Impl so pipeline rebuilds preserve the host's runtime choice.
    bool                           fxaaEnabled = true;
    bool                           smaaEnabled = false;

    // Optional display-referred LUT grading. Disabled by default so all
    // pre-grading hosts retain a zero-allocation, byte-identical path.
    bool                           colorGradingEnabled = false;
    float                          colorGradingStrength = 0.75f;
    ColorGradingPreset             colorGradingPreset = ColorGradingPreset::Warm;

    void applyEditorOverlayKnobs()
    {
        if (detail::RenderPass* overlay = pipeline.findPass("EditorOverlay")) {
            auto* editorOverlay =
                static_cast<detail::EditorOverlayPass*>(overlay);
            editorOverlay->setOrientationAxisEnabled(
                viewportOrientationAxisEnabled);
            editorOverlay->setTransformGizmoState(editorTransformGizmo);
        }
    }

    void applyAntiAliasingKnobs()
    {
        if (detail::RenderPass* fxaa = pipeline.findPass("FXAA")) {
            fxaa->setEnabled(fxaaEnabled);
        }
        if (detail::RenderPass* smaa = pipeline.findPass("SMAA")) {
            smaa->setEnabled(smaaEnabled);
        }
    }

    void applyColorGradingKnobs()
    {
        if (detail::RenderPass* grading = pipeline.findPass("ColorGrading")) {
            grading->setEnabled(colorGradingEnabled);
            auto* pass = static_cast<detail::ColorGradingPass*>(grading);
            pass->setStrength(colorGradingStrength);
            pass->setPreset(colorGradingPreset);
        }
    }

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

    // §P1 H3 (2026-08-24) — shared alive flag. The ResourceManager
    // hot-reload lambda captures the shared_ptr by value and checks
    // `*aliveToken` (atomic load) before touching any Impl member.
    // shutdown() flips it to false before destroying bgfx resources
    // so a callback that races past setOnHotReload({}) sees the dead
    // flag and skips the draw-side work (no UAF on the dying
    // Renderer). Lifetime is owned by Impl; Renderer is the sole
    // producer.
    std::shared_ptr<std::atomic<bool>> aliveToken;

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
        // §P1 H3 (2026-08-24) — alive token shared with the
        // ResourceManager hot-reload lambda. shutdown() flips it to
        // false before destroying bgfx resources so a callback that
        // races past setOnHotReload({}) sees the dead flag and skips
        // the draw-side work (no UAF on the dying Renderer).
        aliveToken = std::make_shared<std::atomic<bool>>(true);
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
    // Presentation is a mandatory boundary for any pipeline containing
    // PostProcess. Auto-insert it for source compatibility with custom
    // descriptors created before RenderPassSlot::Present was appended.
    ensurePresentAfterPostProcess(resolved);

    UIRenderBackend* retainedUi = nullptr;
    if (detail::RenderPass* uiPass = pipeline.findPass("UI")) {
        retainedUi = static_cast<detail::UIPass*>(uiPass)->backend();
    }

    // Transparent's deferred composite FBO borrows Lighting color + GBuffer
    // depth. Release the FBO shell before either producer rotates handles.
    if (detail::RenderPass* transparentPass = pipeline.findPass("Transparent")) {
        if (adapter.isInitialized()) {
            static_cast<detail::TransparentPass*>(transparentPass)
                ->destroyResources(adapter);
        }
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
    // 1× RGBA16F LightingOutput FBO + fullscreen triangle VB/IB +
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
    // above). BloomExtractPass owns a half-resolution RGBA16F FBO
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
    // two half-resolution RGBA16F ping-pong FBOs (no depth) +
    // fullscreen-triangle VB/IB + Phoskia blur program; all four
    // must be released BEFORE pipeline.clear() for the same
    // handle-rotation reason.
    if (detail::RenderPass* bloomBlurPass = pipeline.findPass("BloomBlur")) {
        if (adapter.isInitialized()) {
            static_cast<detail::BloomBlurPass*>(bloomBlurPass)->destroyResources(adapter);
        }
    }

    // DepthHaze owns only fullscreen geometry and its Phoskia program; the
    // full-resolution HazeColor target is FrameGraph-owned. Release the
    // pass-local GPU objects before pipeline.clear().
    if (detail::RenderPass* depthHazePass = pipeline.findPass("DepthHaze")) {
        if (adapter.isInitialized()) {
            static_cast<detail::DepthHazePass*>(depthHazePass)->destroyResources(adapter);
        }
    }

    if (detail::RenderPass* ssaoPass = pipeline.findPass("SSAO")) {
        if (adapter.isInitialized()) {
            static_cast<detail::SSAOPass*>(ssaoPass)->destroyResources(adapter);
        }
    }

    if (detail::RenderPass* debugPass = pipeline.findPass("GBufferDebug")) {
        if (adapter.isInitialized()) {
            static_cast<detail::GBufferDebugPass*>(debugPass)
                ->destroyResources(adapter);
        }
    }

    if (detail::RenderPass* postProcessPass =
            pipeline.findPass("PostProcess")) {
        if (adapter.isInitialized()) {
            static_cast<detail::PostProcessPass*>(postProcessPass)
                ->destroyResources(adapter);
        }
    }

    if (detail::RenderPass* fxaaPass = pipeline.findPass("FXAA")) {
        if (adapter.isInitialized()) {
            static_cast<detail::FXAAPass*>(fxaaPass)
                ->destroyResources(adapter);
        }
    }

    if (detail::RenderPass* smaaPass = pipeline.findPass("SMAA")) {
        if (adapter.isInitialized()) {
            static_cast<detail::SMAAPass*>(smaaPass)
                ->destroyResources(adapter);
        }
    }

    if (detail::RenderPass* gradingPass = pipeline.findPass("ColorGrading")) {
        if (adapter.isInitialized()) {
            static_cast<detail::ColorGradingPass*>(gradingPass)
                ->destroyResources(adapter);
        }
    }

    if (detail::RenderPass* presentPass = pipeline.findPass("Present")) {
        if (adapter.isInitialized()) {
            static_cast<detail::PresentPass*>(presentPass)
                ->destroyResources(adapter);
        }
    }

    if (detail::RenderPass* editorOverlayPass =
            pipeline.findPass("EditorOverlay")) {
        if (adapter.isInitialized()) {
            static_cast<detail::EditorOverlayPass*>(editorOverlayPass)
                ->destroyResources(adapter);
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
    applyAntiAliasingKnobs();
    applyColorGradingKnobs();
    applyEditorOverlayKnobs();
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

    // Engine coordinate-convention sanity. CoordinateConvention.h bakes the
    // contract (LH / Y-up / Z-forward / CCW / V-top) as a static_assert on
    // every TU that includes it; here we re-verify at runtime so the check
    // survives across the linker boundary (the static_assert can be defeated
    // by a stale cached .obj). Bail loudly in debug; release stays quiet
    // because the contract is fixed at compile time and the cost of a
    // spurious failure on a deployed build outweighs the diagnostic value.
#ifndef NDEBUG
    if (!ayt::math::EngineCoordinateConvention::validate()) {
        std::fprintf(stderr,
                     "[Renderer] FATAL: EngineCoordinateConvention::validate() "
                     "failed at runtime; cacheTag='%s'.\n"
                     "[Renderer]   Someone edited CoordinateConvention.h and "
                     "broke the engine contract. Restore LH / Y-up / Z-forward "
                     "/ CCW / V-top or bump cacheTag.\n",
                     ayt::math::EngineCoordinateConvention::cacheTag);
        std::abort();
    }
#endif

    detail::BGFXInitParams bgfxParams;
    bgfxParams.nativeWindowHandle = desc.windowHandle;
    bgfxParams.width              = desc.width;
    bgfxParams.height             = desc.height;
    bgfxParams.vsync              = desc.vsync;
    bgfxParams.backend            = desc.backend;
    bgfxParams.msaa               = desc.msaa;
    if (const std::string msaaEnv = ayt::io::env::get("AY_MSAA").value_or(""); !msaaEnv.empty()) {
        bgfxParams.msaa = static_cast<uint32_t>(std::atoi(msaaEnv.c_str()));
    }

    if (!_impl->adapter.initialize(bgfxParams)) {
        return false;
    }

    _impl->initDesc        = desc;
    _impl->initDesc.msaa   = bgfxParams.msaa;
    _impl->shadowPcfEnabled = desc.shadowPcf;
    if (const std::string pcfEnv = ayt::io::env::get("AY_SHADOW_PCF").value_or(""); !pcfEnv.empty()) {
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

    // P2: L2 file change → L3 GPU re-upload (stable handle ids).
    // §P1 H3 (2026-08-24) — capture the alive token (shared_ptr) and
    // check it before dereferencing Impl. If the Renderer was destroyed
    // and the callback raced past setOnHotReload({}), the token will
    // be false and we skip silently instead of touching dead memory.
    // A Renderer may be initialized again after shutdown(). Give the new
    // callback its own live token; callbacks copied before shutdown retain
    // the old, permanently-false token and remain safely disabled.
    if (!_impl->aliveToken
        || !_impl->aliveToken->load(std::memory_order_acquire)) {
        _impl->aliveToken = std::make_shared<std::atomic<bool>>(true);
    }
    auto aliveToken = _impl->aliveToken;
    ayt::resource::ResourceManager::instance().setOnHotReload(
        [this, aliveToken](const std::string& path) {
            if (!aliveToken || !aliveToken->load(std::memory_order_acquire)) {
                return;
            }
            if (_impl) {
                (void)_impl->resources.onResourceFileChanged(path);
            }
        });
    return true;
}

void Renderer::shutdown()
{
    if (!_impl) {
        return;
    }

    // §P1 H3 (2026-08-24) — flip the alive flag FIRST so any hot-reload
    // callback racing past setOnHotReload({}) sees the dead state. Must
    // run before any bgfx resource destruction so the callback never
    // touches a half-torn-down Impl.
    if (_impl->aliveToken) {
        _impl->aliveToken->store(false, std::memory_order_release);
    }

    if (detail::RenderPass* transparentPass =
            _impl->pipeline.findPass("Transparent")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::TransparentPass*>(transparentPass)
                ->destroyResources(_impl->adapter);
        }
    }

    // Pipeline instances survive shutdown() so the same Renderer can be
    // initialized again. Reset every Pass-owned GPU handle before bgfx shuts
    // down; bgfx::isValid only checks the numeric handle sentinel and cannot
    // distinguish a stale handle from one allocated by the next context.
    if (detail::RenderPass* shadowPass = _impl->pipeline.findPass("Shadow")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::ShadowPass*>(shadowPass)
                ->destroyResources(_impl->adapter);
        }
    }
    if (detail::RenderPass* gbufferPass = _impl->pipeline.findPass("GBuffer")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::GBufferPass*>(gbufferPass)
                ->destroyResources(_impl->adapter);
        }
    }
    if (detail::RenderPass* lightingPass = _impl->pipeline.findPass("Lighting")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::LightingPass*>(lightingPass)
                ->destroyResources(_impl->adapter);
        }
    }
    if (detail::RenderPass* skyboxPass = _impl->pipeline.findPass("Skybox")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::SkyboxPass*>(skyboxPass)
                ->destroyResources(_impl->adapter);
        }
    }
    if (detail::RenderPass* bloomExtractPass =
            _impl->pipeline.findPass("BloomExtract")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::BloomExtractPass*>(bloomExtractPass)
                ->destroyResources(_impl->adapter);
        }
    }
    if (detail::RenderPass* bloomBlurPass =
            _impl->pipeline.findPass("BloomBlur")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::BloomBlurPass*>(bloomBlurPass)
                ->destroyResources(_impl->adapter);
        }
    }
    if (detail::RenderPass* depthHazePass =
            _impl->pipeline.findPass("DepthHaze")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::DepthHazePass*>(depthHazePass)
                ->destroyResources(_impl->adapter);
        }
    }
    if (detail::RenderPass* debugPass =
            _impl->pipeline.findPass("GBufferDebug")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::GBufferDebugPass*>(debugPass)
                ->destroyResources(_impl->adapter);
        }
    }

    // PostProcess owns raw fullscreen geometry and a ShaderResource. Clear
    // both while the adapter and shader pool are still alive so a later
    // initialize() cannot mistake stale numeric handles for live objects.
    if (detail::RenderPass* postProcessPass =
            _impl->pipeline.findPass("PostProcess")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::PostProcessPass*>(postProcessPass)
                ->destroyResources(_impl->adapter);
        }
    }

    if (detail::RenderPass* fxaaPass =
            _impl->pipeline.findPass("FXAA")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::FXAAPass*>(fxaaPass)
                ->destroyResources(_impl->adapter);
        }
    }

    if (detail::RenderPass* smaaPass =
            _impl->pipeline.findPass("SMAA")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::SMAAPass*>(smaaPass)
                ->destroyResources(_impl->adapter);
        }
    }

    if (detail::RenderPass* gradingPass =
            _impl->pipeline.findPass("ColorGrading")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::ColorGradingPass*>(gradingPass)
                ->destroyResources(_impl->adapter);
        }
    }

    if (detail::RenderPass* presentPass =
            _impl->pipeline.findPass("Present")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::PresentPass*>(presentPass)
                ->destroyResources(_impl->adapter);
        }
    }

    if (detail::RenderPass* editorOverlayPass =
            _impl->pipeline.findPass("EditorOverlay")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::EditorOverlayPass*>(editorOverlayPass)
                ->destroyResources(_impl->adapter);
        }
    }

    // SSAO keeps raw fullscreen geometry handles in the pass instance. Reset
    // them before adapter shutdown so initialize() on the same Renderer never
    // mistakes stale numeric handles for live resources.
    if (detail::RenderPass* ssaoPass = _impl->pipeline.findPass("SSAO")) {
        if (_impl->adapter.isInitialized()) {
            static_cast<detail::SSAOPass*>(ssaoPass)
                ->destroyResources(_impl->adapter);
        }
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

    // §F6 (2026-07-24) — release FrameGraph-owned transient RTs
    // before tearing down the adapter. Mirrors the sceneFbo block
    // above. fg.shutdown() iterates all owned resources and
    // destroys each (externals are skipped). Idempotent.
    _impl->frameGraph.shutdown();
    _impl->renderTargetPool.shutdown();

    ayt::resource::ResourceManager::instance().setOnHotReload({});
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
    _impl->pipeline.resetFrameStats();
    _impl->debugOverlay.onBeginFrame();
    _impl->renderTargetPool.beginFrame();
    _impl->adapter.beginFrame();
    _impl->adapter.setViewClear(detail::ForwardOpaquePass::kMainViewId, clear);
}

void Renderer::beginCompositeFrame(const ClearDesc& clear, uint16_t fbWidth, uint16_t fbHeight)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }
    _impl->pipeline.resetFrameStats();
    _impl->debugOverlay.onBeginFrame();
    _impl->renderTargetPool.beginFrame();

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
    frame.bloomStrength    = detail::sanitizeBloomStrength(
        _impl->postProcessBloomStrength);
    frame.exposure         = detail::sanitizePostProcessExposure(
        _impl->postProcessExposure);
    frame.gamma            = detail::sanitizePostProcessGamma(
        _impl->postProcessGamma);
    frame.tonemapMode      = _impl->postProcessTonemapMode;
    // §S4d — DepthHaze knobs (Editor / host). Defaults keep haze off.
    frame.hazeEnabled      = _impl->depthHazeEnabled;
    frame.hazeStrength     = detail::sanitizeDepthHazeStrength(
        _impl->depthHazeStrength);
    frame.hazeDensity      = detail::sanitizeDepthHazeDensity(
        _impl->depthHazeDensity);
    frame.hazeColor        = detail::sanitizeDepthHazeColor(
        _impl->depthHazeColor);
    // §S2 v1 — SSAO knobs (Editor / host). Defaults keep SSAO off.
    frame.ssaoEnabled      = _impl->ssaoEnabled;
    frame.ssaoStrength     = detail::sanitizeSsaoStrength(_impl->ssaoStrength);
    frame.ssaoRadius       = detail::sanitizeSsaoRadius(_impl->ssaoRadius);
    frame.ssaoBias         = std::min(
        detail::sanitizeSsaoBias(_impl->ssaoBias), frame.ssaoRadius);
    // GBuffer attachment overlay knobs. Disabled is a zero-allocation path;
    // enabled rendering happens in GBufferDebugPass on view 250.
    frame.gbufferDebugEnabled = _impl->gbufferDebugEnabled;
    frame.gbufferDebugChannel = _impl->gbufferDebugChannel;
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

    // Scene FBO for FO/Transparent → PostProcess FinalLdrColor → FXAA → Present.
    // Editor composite uses the same panel-sized offscreen scene path:
    // PostProcess writes viewport-local FinalLdrColor on view 15, then
    // Present applies (vx,vy,w,h) to the default backbuffer on view 16.
    const bgfx::FrameBufferHandle sceneFbo = _impl->ensureSceneFbo();

    // §P5 B4b (2026-07-22) — broadcast viewport size to GBufferPass
    // before dispatch so its execute() can ensure() the 4-attach
    // MRT FBO at the correct W×H. Mirror the sceneFbo / viewportW
    // wiring above (size is the same panel rect for the Deferred
    // path). Skipped when GBuffer isn't in the configured pipeline
    // (Forward path) — cutsheet §4.1 red line #4.
    if (detail::RenderPass* gbufferSlot = _impl->pipeline.findPass("GBuffer")) {
        static_cast<detail::GBufferPass*>(gbufferSlot)
            ->setGbufferSize(_impl->viewportW, _impl->viewportH);
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
    // before dispatch so its execute() can ensure() the 1× RGBA16F
    // LightingOutput FBO at the correct W×H. Mirror the GBuffer
    // setGbufferSize block above (same viewport rect). Skipped
    // when Lighting isn't in the configured pipeline (Forward path)
    // — cutsheet §4.1 red line #4.
    if (detail::RenderPass* lightingSlot = _impl->pipeline.findPass("Lighting")) {
        auto* lighting = static_cast<detail::LightingPass*>(lightingSlot);
        lighting->setOutputSize(_impl->viewportW, _impl->viewportH);
        // FrameGraph imports SceneColor below, before pass dispatch. Ensure
        // the lazy Lighting output exists now so cold-start and post-resize
        // frames import the deferred source instead of the forward sceneFbo.
        lighting->prepareOutput(_impl->adapter);
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
        static_cast<detail::SkyboxPass*>(skyboxSlot)
            ->setOutputSize(_impl->viewportW, _impl->viewportH);
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
    detail::GBufferPass* gbufferPassPtr = nullptr;
    if (detail::RenderPass* gbufferSlot = _impl->pipeline.findPass("GBuffer")) {
        gbufferPassPtr = static_cast<detail::GBufferPass*>(gbufferSlot);
        gbufferPassPtr->resetFrameState();
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
    detail::LightingPass* lightingPassPtr = nullptr;
    if (detail::RenderPass* lightingSlot = _impl->pipeline.findPass("Lighting")) {
        lightingPassPtr = static_cast<detail::LightingPass*>(lightingSlot);
        lightingPassPtr->resetFrameState();
    }

    // §Skybox0 (2026-07-23) — borrowed pointer to the SkyboxPass in
    // the pipeline. nullptr when the host did not opt in via
    // `configurePipeline(makeDeferred())` (Forward / no skybox path).
    const detail::SkyboxPass* skyboxPassPtr = nullptr;
    if (detail::RenderPass* skyboxSlot = _impl->pipeline.findPass("Skybox")) {
        skyboxPassPtr = static_cast<const detail::SkyboxPass*>(skyboxSlot);
    }

    // Borrow both bloom passes and clear their production latches before graph
    // compilation. Downstream consumers require a successful submit from this
    // frame, never merely a still-valid attachment handle.
    detail::BloomExtractPass* bloomExtractPassPtr = nullptr;
    if (detail::RenderPass* bloomExtractSlot = _impl->pipeline.findPass("BloomExtract")) {
        bloomExtractPassPtr = static_cast<detail::BloomExtractPass*>(bloomExtractSlot);
        bloomExtractPassPtr->resetFrameState();
    }

    detail::BloomBlurPass* bloomBlurPassPtr = nullptr;
    if (detail::RenderPass* bloomBlurSlot = _impl->pipeline.findPass("BloomBlur")) {
        bloomBlurPassPtr = static_cast<detail::BloomBlurPass*>(bloomBlurSlot);
        bloomBlurPassPtr->resetFrameState();
    }

    // Borrow the haze producer so downstream passes can require a successful
    // submit from this frame before promoting FgSemantic::HazeSource.
    detail::DepthHazePass* depthHazePassPtr = nullptr;
    if (detail::RenderPass* depthHazeSlot = _impl->pipeline.findPass("DepthHaze")) {
        depthHazePassPtr = static_cast<detail::DepthHazePass*>(depthHazeSlot);
        depthHazePassPtr->resetFrameState();
    }

    detail::SSAOPass* ssaoPassPtr = nullptr;
    if (detail::RenderPass* ssaoSlot = _impl->pipeline.findPass("SSAO")) {
        ssaoPassPtr = static_cast<detail::SSAOPass*>(ssaoSlot);
        ssaoPassPtr->resetFrameState();
    }

    detail::PostProcessPass* postProcessPassPtr = nullptr;
    if (detail::RenderPass* postProcessSlot =
            _impl->pipeline.findPass("PostProcess")) {
        postProcessPassPtr =
            static_cast<detail::PostProcessPass*>(postProcessSlot);
    }
    detail::FXAAPass* fxaaPassPtr = nullptr;
    if (detail::RenderPass* fxaaSlot =
            _impl->pipeline.findPass("FXAA")) {
        fxaaPassPtr = static_cast<detail::FXAAPass*>(fxaaSlot);
    }
    detail::SMAAPass* smaaPassPtr = nullptr;
    if (detail::RenderPass* smaaSlot =
            _impl->pipeline.findPass("SMAA")) {
        smaaPassPtr = static_cast<detail::SMAAPass*>(smaaSlot);
    }
    detail::ColorGradingPass* colorGradingPassPtr = nullptr;
    if (detail::RenderPass* gradingSlot =
            _impl->pipeline.findPass("ColorGrading")) {
        colorGradingPassPtr =
            static_cast<detail::ColorGradingPass*>(gradingSlot);
    }
    detail::PresentPass* presentPassPtr = nullptr;
    if (detail::RenderPass* presentSlot =
            _impl->pipeline.findPass("Present")) {
        presentPassPtr = static_cast<detail::PresentPass*>(presentSlot);
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

    // Build the post-process graph before any pass resolves a target. Bloom is
    // one capability: finite positive strength plus both mounted/enabled
    // stages. Otherwise all three transient targets are omitted.
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

    const bool backendReady = _impl->adapter.isInitialized()
        && !_impl->adapter.isNoopBackend();
    const bool gbufferStagePresent = gbufferPassPtr != nullptr
        && gbufferPassPtr->isEnabled();
    const bool lightingStagePresent = lightingPassPtr != nullptr
        && lightingPassPtr->isEnabled();

    // Lighting is the sole SSAO consumer. The graph allocates the target only
    // when the complete GBuffer -> SSAO -> Lighting chain is available.
    const bool ssaoPassEnabled = detail::selectSsaoStage(
        frame.ssaoEnabled,
        frame.ssaoStrength,
        frame.ssaoRadius,
        ssaoPassPtr != nullptr,
        ssaoPassPtr != nullptr && ssaoPassPtr->isEnabled(),
        gbufferPassPtr != nullptr,
        gbufferStagePresent,
        lightingPassPtr != nullptr,
        lightingStagePresent,
        fgW,
        fgH) && backendReady;
    if (ssaoPassEnabled) {
        fg.addResource(detail::FgResourceId::SSAOTexture,
                       {bgfx::TextureFormat::RGBA8,
                        detail::FgTextureScale::Full,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addPass({"SSAO",
                    {},
                    {detail::FgResourceId::SSAOTexture},
                    /*enabled=*/true});
        fg.setResolvedSemantic(detail::FgSemantic::SSAOSource,
                               detail::FgResourceId::SSAOTexture);
    }

    const bool hazePassEnabled = detail::selectDepthHazeStage(
        frame.hazeEnabled,
        frame.hazeStrength,
        depthHazePassPtr != nullptr,
        depthHazePassPtr != nullptr && depthHazePassPtr->isEnabled(),
        gbufferPassPtr != nullptr && gbufferPassPtr->isEnabled(),
        lightingPassPtr != nullptr && lightingPassPtr->isEnabled(),
        fgW,
        fgH) && backendReady;
    if (hazePassEnabled) {
        fg.addResource(detail::FgResourceId::HazeColor,
                       {detail::kHdrSceneColorFormat,
                        detail::FgTextureScale::Full,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addPass({"DepthHaze",
                    {detail::FgResourceId::SceneColor},
                    {detail::FgResourceId::HazeColor},
                    /*enabled=*/true});
        fg.setResolvedSemantic(detail::FgSemantic::HazeSource,
                               detail::FgResourceId::HazeColor);
    }

    const detail::FgResourceId hdrSceneSource = hazePassEnabled
        ? detail::FgResourceId::HazeColor
        : detail::FgResourceId::SceneColor;
    const detail::BloomStageState bloomStages = detail::selectBloomStages(
        frame.bloomStrength,
        bloomExtractPassPtr != nullptr,
        bloomExtractPassPtr != nullptr && bloomExtractPassPtr->isEnabled(),
        bloomBlurPassPtr != nullptr,
        bloomBlurPassPtr != nullptr && bloomBlurPassPtr->isEnabled());
    if (bloomStages.extract) {
        fg.addResource(detail::FgResourceId::BloomBright,
                       {detail::kHdrSceneColorFormat,
                        detail::FgTextureScale::Half,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addPass({"BloomExtract",
                    {hdrSceneSource},
                    {detail::FgResourceId::BloomBright},
                    /*enabled=*/true});
    }
    if (bloomStages.blur) {
        fg.addResource(detail::FgResourceId::BloomBlurA,
                       {detail::kHdrSceneColorFormat,
                        detail::FgTextureScale::Half,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addResource(detail::FgResourceId::BloomBlurB,
                       {detail::kHdrSceneColorFormat,
                        detail::FgTextureScale::Half,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addPass({"BloomBlurH",
                    {detail::FgResourceId::BloomBright},
                    {detail::FgResourceId::BloomBlurA},
                    /*enabled=*/true});
        fg.addPass({"BloomBlurV",
                    {detail::FgResourceId::BloomBlurA},
                    {detail::FgResourceId::BloomBlurB},
                    /*enabled=*/true});
        fg.setResolvedSemantic(detail::FgSemantic::BloomSource,
                               detail::FgResourceId::BloomBlurB);
    }

    // FinalColorSource always names the stable underlying scene. Runtime source
    // selection promotes HazeSource only after its current-frame latch is set.
    fg.setResolvedSemantic(detail::FgSemantic::FinalColorSource,
                           detail::FgResourceId::SceneColor);

    // FinalPP is an offscreen producer and Present is the sole owner of the
    // default-backbuffer boundary. PresentSource starts at FinalLdrColor each
    // frame; FXAA and ColorGrading promote it transactionally only after
    // their successful submits.
    const bool finalLdrStageEnabled = backendReady
        && fgW != 0 && fgH != 0
        && postProcessPassPtr != nullptr
        && postProcessPassPtr->isEnabled()
        && presentPassPtr != nullptr
        && presentPassPtr->isEnabled();
    if (finalLdrStageEnabled) {
        fg.addResource(detail::FgResourceId::FinalLdrColor,
                       {bgfx::TextureFormat::RGBA8,
                        detail::FgTextureScale::Full,
                        /*transient=*/true,
                        /*withDepth=*/false});
        fg.addPass({"PostProcess",
                    {hdrSceneSource},
                    {detail::FgResourceId::FinalLdrColor},
                    /*enabled=*/true});
        const bool fxaaStageEnabled = fxaaPassPtr != nullptr
            && fxaaPassPtr->isEnabled();
        if (fxaaStageEnabled) {
            fg.addResource(detail::FgResourceId::FxaaColor,
                           {bgfx::TextureFormat::RGBA8,
                            detail::FgTextureScale::Full,
                            /*transient=*/true,
                            /*withDepth=*/false});
            fg.addPass({"FXAA",
                        {detail::FgResourceId::FinalLdrColor},
                        {detail::FgResourceId::FxaaColor},
                        /*enabled=*/true});
        }
        const bool smaaStageEnabled = smaaPassPtr != nullptr
            && smaaPassPtr->isEnabled();
        if (smaaStageEnabled) {
            fg.addResource(detail::FgResourceId::SmaaEdges,
                           {bgfx::TextureFormat::RGBA8,
                            detail::FgTextureScale::Full,
                            /*transient=*/true,
                            /*withDepth=*/false,
                            detail::SMAAPass::kIntermediatePointSampled});
            fg.addResource(detail::FgResourceId::SmaaBlendWeights,
                           {bgfx::TextureFormat::RGBA8,
                            detail::FgTextureScale::Full,
                            /*transient=*/true,
                            /*withDepth=*/false,
                            detail::SMAAPass::kIntermediatePointSampled});
            fg.addResource(detail::FgResourceId::SmaaColor,
                           {bgfx::TextureFormat::RGBA8,
                            detail::FgTextureScale::Full,
                            /*transient=*/true,
                            /*withDepth=*/false});
            fg.addPass({"SMAA",
                        {fxaaStageEnabled
                             ? detail::FgResourceId::FxaaColor
                             : detail::FgResourceId::FinalLdrColor},
                        {detail::FgResourceId::SmaaEdges,
                         detail::FgResourceId::SmaaBlendWeights,
                         detail::FgResourceId::SmaaColor},
                        /*enabled=*/true});
        }
        const bool colorGradingStageEnabled = colorGradingPassPtr != nullptr
            && colorGradingPassPtr->isEnabled()
            && colorGradingPassPtr->strength() > 0.0f
            && colorGradingPassPtr->preset() != ColorGradingPreset::Neutral;
        if (colorGradingStageEnabled) {
            fg.addResource(detail::FgResourceId::ColorGradedColor,
                           {bgfx::TextureFormat::RGBA8,
                            detail::FgTextureScale::Full,
                            /*transient=*/true,
                            /*withDepth=*/false});
            fg.addPass({"ColorGrading",
                        {smaaStageEnabled
                             ? detail::FgResourceId::SmaaColor
                             : (fxaaStageEnabled
                                    ? detail::FgResourceId::FxaaColor
                                    : detail::FgResourceId::FinalLdrColor)},
                        {detail::FgResourceId::ColorGradedColor},
                        /*enabled=*/true});
        }
        fg.addPass({"Present",
                    {colorGradingStageEnabled
                         ? detail::FgResourceId::ColorGradedColor
                         : (smaaStageEnabled
                                ? detail::FgResourceId::SmaaColor
                                : (fxaaStageEnabled
                                       ? detail::FgResourceId::FxaaColor
                                       : detail::FgResourceId::FinalLdrColor))},
                    {},
                    /*enabled=*/true});
        fg.setResolvedSemantic(detail::FgSemantic::PresentSource,
                               detail::FgResourceId::FinalLdrColor);
    }

    // GBufferDebug now overlays the selected attachment directly into the
    // game viewport on view 250. No hidden host-owned FBO is allocated; UI
    // view 255 still renders afterwards, so Editor chrome remains intact.

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
        // Bloom producer latches; physical textures resolve through FG.
        bloomExtractPassPtr,
        bloomBlurPassPtr,
        // Haze producer latch. HazeColor resolves through FrameGraph; nullptr
        // or a false current-frame latch fails closed to the underlying scene.
        depthHazePassPtr,
        // Renderer-owned FrameGraph for Bloom, HazeColor and SSAO transients.
        &_impl->frameGraph,
        detail::sanitizeBloomThreshold(_impl->postProcessBloomThreshold),
        detail::sanitizeBloomSoftKnee(_impl->postProcessBloomSoftKnee),
        ssaoPassPtr,
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
    // [ForwardOpaque, Transparent, PostProcess, FXAA, Present, UI].
    // ForwardOpaquePass
    // writes the depth buffer first; TransparentPass reuses that depth
    // with DEPTH_TEST_LEQUAL but does not WRITE_Z. Transparent submission is
    // stable back-to-front: explicit DrawItem::sortKey first, camera distance
    // second, with Sequential view mode preserving that CPU order.
    // PostProcessPass writes FrameGraph FinalLdrColor; FXAA may promote
    // PresentSource to FxaaColor, then PresentPass samples the current source
    // into the Game View backbuffer rect. UIPass ignores the viewId arg and
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
    detail::configureRenderViewOrder(_impl->adapter);
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
    if (detail::RenderPass* transparentPass =
            _impl->pipeline.findPass("Transparent")) {
        static_cast<detail::TransparentPass*>(transparentPass)
            ->destroyResources(_impl->adapter);
    }
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
    // §F6 (2026-07-24, mid-term FG MVP sub-cut 6) — PostProcessPass
    // FBO destroy block removed. PostProcessPass is a thin blit pass
    // and FBO lifecycle was historical (pre-P2 the pass had its own
    // color+depth FBO). Today (P2 + B6 closure) it samples the
    // LightingOutputFbo / sceneFbo directly and no longer maintains
    // a private RT. F6 confirms that pattern by removing the
    // destroyResources call.
    // (S1a / S1b / S4b individual passes' destroyResources blocks
    // also removed ── those passes no longer own FBOs in F2/F3/F4,
    // so calling destroyResources would be a no-op-but-noisy. F6
    // consolidates: only VB/IB + program cleanup, no RT destroy.)
    //
    // §F6 — centralized FrameGraph resize. All post-process chain
    // transient RTs (BloomBright / BloomBlurA / BloomBlurB /
    // HazeColor / SSAOTexture) live on the FrameGraph now; resize() destroys
    // owned FG RTs, leaves externals alone (mirror sceneFbo
    // block above). Passes (BloomExtract / BloomBlur / DepthHaze
    // / PostProcess) get a single fg.resize() call instead of
    // four individual destroyResources blocks.
    _impl->frameGraph.resize(width, height);
    // A bgfx reset invalidates retained framebuffer storage. FrameGraph has
    // returned its leases above; invalidate UI/other leases before reset.
    _impl->renderTargetPool.reset();

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
    sceneFbo = adapter.createColorDepthFrameBuffer(
        static_cast<uint16_t>(w), static_cast<uint16_t>(h),
        detail::kHdrSceneColorFormat,
        /*pointSampled=*/false);
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
                                   _impl->pipeline.lastPassStats(),
                                   _impl->viewportX, _impl->viewportY,
                                   _impl->viewportW, _impl->viewportH);
    _impl->adapter.endFrame();
    _impl->debugOverlay.onFrameSubmitted();
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

bool Renderer::hasMorphTargets(MeshHandle mesh) const noexcept
{
    return _impl != nullptr && _impl->resources.hasMorphTargets(mesh);
}

uint32_t Renderer::morphTargetCount(MeshHandle mesh) const noexcept
{
    return _impl != nullptr ? _impl->resources.morphTargetCount(mesh) : 0u;
}

std::string Renderer::morphTargetName(MeshHandle mesh, uint32_t targetIndex) const
{
    return _impl != nullptr
        ? _impl->resources.morphTargetName(mesh, targetIndex)
        : std::string{};
}

bool Renderer::setMorphWeight(MeshHandle mesh, uint32_t targetIndex, float weight)
{
    return _impl != nullptr && _impl->resources.setMorphWeight(mesh, targetIndex, weight);
}

bool Renderer::setMorphWeight(MeshHandle mesh,
                              const std::string& targetName,
                              float weight)
{
    return _impl != nullptr && _impl->resources.setMorphWeight(mesh, targetName, weight);
}

float Renderer::morphWeight(MeshHandle mesh, uint32_t targetIndex) const noexcept
{
    return _impl != nullptr ? _impl->resources.morphWeight(mesh, targetIndex) : 0.0f;
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

MeshHandle Renderer::createUnitQuad()
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createUnitQuad();
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

TextureHandle Renderer::createDynamicTextureRgba8(uint32_t width, uint32_t height)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createDynamicTextureRgba8(width, height);
}

bool Renderer::updateTextureFromRgba8(TextureHandle texture, const uint8_t* pixels)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return false;
    }
    return _impl->resources.updateTextureFromRgba8(texture, pixels);
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
    // §P1 H2 (2026-08-24) — route through RenderResourceManager so the
    // dirty mark + pass-routing comment lands with the change instead
    // of poking the internal map directly (which was a cutsheet §6
    // violation: passes must not access GPU resource state directly).
    if (!_impl) {
        return;
    }
    (void)_impl->resources.setMaterialBlendMode(material, blendMode);
}

void Renderer::setMaterialModel(MaterialHandle material, MaterialModel model)
{
    if (!_impl || !material.isValid()) {
        return;
    }
    (void)_impl->resources.setMaterialModel(material, model);
}

void Renderer::setMaterialSurfaceProperties(MaterialHandle material, int alphaMode,
                                             float alphaCutoff, bool doubleSided)
{
    if (!_impl) {
        return;
    }
    _impl->resources.setMaterialSurfaceProperties(material, alphaMode,
                                                   alphaCutoff, doubleSided);
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
    uint32_t requestedSamples = 0;
    switch (samples) {
    case 2: case 4: case 8: case 16:
        requestedSamples = samples;
        break;
    default:
        break;
    }
    const uint32_t before = _impl->adapter.msaaSampleCount();
    // BGFXAdapter::setMsaaSampleCount performs bgfx::reset immediately.
    // Release every framebuffer owner first so retained UI/FrameGraph leases
    // cannot keep a numerically-valid but device-stale handle afterward.
    if (before != requestedSamples) {
        if (detail::BGFXAdapter::isValid(_impl->sceneFbo)) {
            _impl->adapter.destroy(_impl->sceneFbo);
            _impl->sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        }
        _impl->sceneFboW = 0;
        _impl->sceneFboH = 0;
        if (detail::RenderPass* transparentPass =
                _impl->pipeline.findPass("Transparent")) {
            if (_impl->adapter.isInitialized()) {
                static_cast<detail::TransparentPass*>(transparentPass)
                    ->destroyResources(_impl->adapter);
            }
        }
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
        _impl->frameGraph.resize(_impl->initDesc.width, _impl->initDesc.height);
        _impl->renderTargetPool.reset();
        _impl->adapter.setMsaaSampleCount(requestedSamples);
        _impl->initDesc.msaa = _impl->adapter.msaaSampleCount();
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

void Renderer::setShadowsEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    if (detail::RenderPass* shadow = _impl->pipeline.findPass("Shadow")) {
        shadow->setEnabled(enabled);
    }
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
    // Values above one remain valid artistic intensities. Invalid and
    // negative values collapse to zero so the graph and final composite use
    // one finite enable contract.
    _impl->postProcessBloomStrength = detail::sanitizeBloomStrength(strength);
}

void Renderer::setPostProcessBloomThreshold(float threshold)
{
    if (_impl) {
        _impl->postProcessBloomThreshold =
            detail::sanitizeBloomThreshold(threshold);
    }
}

void Renderer::setPostProcessBloomSoftKnee(float softKnee)
{
    if (_impl) {
        _impl->postProcessBloomSoftKnee =
            detail::sanitizeBloomSoftKnee(softKnee);
    }
}

void Renderer::setPostProcessExposure(float exposure)
{
    if (!_impl) {
        return;
    }
    _impl->postProcessExposure =
        detail::sanitizePostProcessExposure(exposure);
}

void Renderer::setPostProcessGamma(float gamma)
{
    if (!_impl) {
        return;
    }
    _impl->postProcessGamma = detail::sanitizePostProcessGamma(gamma);
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

void Renderer::setFxaaEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    _impl->fxaaEnabled = enabled;
    if (enabled) {
        _impl->smaaEnabled = false;
    }
    _impl->applyAntiAliasingKnobs();
}

bool Renderer::fxaaEnabled() const noexcept
{
    return _impl != nullptr && _impl->fxaaEnabled;
}

void Renderer::setSmaaEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    _impl->smaaEnabled = enabled;
    if (enabled) {
        _impl->fxaaEnabled = false;
    }
    _impl->applyAntiAliasingKnobs();
}

bool Renderer::smaaEnabled() const noexcept
{
    return _impl != nullptr && _impl->smaaEnabled;
}

void Renderer::setColorGradingEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    _impl->colorGradingEnabled = enabled;
    _impl->applyColorGradingKnobs();
}

bool Renderer::colorGradingEnabled() const noexcept
{
    return _impl != nullptr && _impl->colorGradingEnabled;
}

void Renderer::setColorGradingStrength(float strength)
{
    if (!_impl) {
        return;
    }
    _impl->colorGradingStrength = std::isfinite(strength)
        ? std::clamp(strength, 0.0f, 1.0f)
        : 0.0f;
    _impl->applyColorGradingKnobs();
}

float Renderer::colorGradingStrength() const noexcept
{
    return _impl ? _impl->colorGradingStrength : 0.0f;
}

void Renderer::setColorGradingPreset(ColorGradingPreset preset)
{
    if (!_impl) {
        return;
    }
    if (static_cast<uint8_t>(preset)
        > static_cast<uint8_t>(ColorGradingPreset::Cinematic)) {
        preset = ColorGradingPreset::Neutral;
    }
    _impl->colorGradingPreset = preset;
    _impl->applyColorGradingKnobs();
}

ColorGradingPreset Renderer::colorGradingPreset() const noexcept
{
    return _impl ? _impl->colorGradingPreset : ColorGradingPreset::Neutral;
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
    _impl->depthHazeStrength = detail::sanitizeDepthHazeStrength(strength);
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
    _impl->depthHazeDensity = detail::sanitizeDepthHazeDensity(density);
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
    _impl->depthHazeColor = detail::sanitizeDepthHazeColor(color);
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

void Renderer::setSsaoEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    _impl->ssaoEnabled = enabled;
}

bool Renderer::ssaoEnabled() const noexcept
{
    return _impl != nullptr && _impl->ssaoEnabled;
}

void Renderer::setSsaoStrength(float strength)
{
    if (!_impl) {
        return;
    }
    _impl->ssaoStrength = detail::sanitizeSsaoStrength(strength);
}

float Renderer::ssaoStrength() const noexcept
{
    return _impl ? _impl->ssaoStrength : 0.0f;
}

void Renderer::setSsaoRadius(float radius)
{
    if (!_impl) {
        return;
    }
    _impl->ssaoRadius = detail::sanitizeSsaoRadius(radius);
}

float Renderer::ssaoRadius() const noexcept
{
    return _impl ? _impl->ssaoRadius : 0.5f;
}

void Renderer::setSsaoBias(float bias)
{
    if (!_impl) {
        return;
    }
    _impl->ssaoBias = detail::sanitizeSsaoBias(bias);
}

float Renderer::ssaoBias() const noexcept
{
    return _impl ? _impl->ssaoBias : 0.025f;
}

void Renderer::setSsaoParams(float radius, float bias)
{
    setSsaoRadius(radius);
    setSsaoBias(bias);
}

// GBuffer attachment-overlay controls. Channel is clamped to the enum range
// so stale host UI state cannot reach the shader selector.
void Renderer::setGBufferDebugEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    _impl->gbufferDebugEnabled = enabled;
}

bool Renderer::gbufferDebugEnabled() const noexcept
{
    return _impl != nullptr && _impl->gbufferDebugEnabled;
}

void Renderer::setGBufferDebugChannel(uint8_t channel)
{
    if (!_impl) {
        return;
    }
    // Clamp into valid range before the branchless shader selector.
    constexpr uint8_t kMax = static_cast<uint8_t>(
        detail::GBufferDebugChannel::Count) - 1;
    _impl->gbufferDebugChannel = (channel > kMax) ? 0 : channel;
}

uint8_t Renderer::gbufferDebugChannel() const noexcept
{
    return _impl ? _impl->gbufferDebugChannel : 0u;
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

    // Engine convention is LH (see AYMath/MathUtils.h lh::). External API
    // surfaces LH Float4x4; BGFXAdapter::setViewTransform converts to
    // column-major bytes at the GPU upload boundary.
    const ayt::math::Float4x4 view = ayt::math::lh::lookAt(eye, at, up);
    // Keep the public API compatible with the old bx::mtxProj call: callers
    // provide degrees, while AYMath projection helpers intentionally accept
    // radians. Passing degrees through directly can make tan(fov/2) negative
    // (for the Editor default 50 degrees), flipping both clip-space X and Y
    // and producing an incorrect focal length.
    const ayt::math::Float4x4 proj =
        detail::makeLeftHandedPerspectiveDegrees(
            fovYDegrees, aspect, nearZ, farZ);

    _impl->mainCameraPosition = eye;
    setMainCamera(view, proj);
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

void Renderer::pollResourceHotReload()
{
    // Manager drains FileWatcher/mtime, invalidates L2, then invokes the
    // callback installed in initialize() which refreshes L3 GPU caches.
    ayt::resource::ResourceManager::instance().update(0.0f);
}

uint32_t Renderer::reloadMaterialsForShaderFile(const std::string& shaderPath)
{
    if (!_impl || !_impl->shaderPoolReady || shaderPath.empty()) {
        return 0;
    }
    return _impl->resources.reloadMaterialsForShaderFile(shaderPath);
}

void Renderer::setViewportOrientationAxisEnabled(bool enabled)
{
    if (!_impl) {
        return;
    }
    _impl->viewportOrientationAxisEnabled = enabled;
    _impl->applyEditorOverlayKnobs();
}

bool Renderer::viewportOrientationAxisEnabled() const noexcept
{
    return _impl && _impl->viewportOrientationAxisEnabled;
}

void Renderer::setEditorTransformGizmoState(
    const EditorTransformGizmoState& state)
{
    if (!_impl) return;
    _impl->editorTransformGizmo = state;
    _impl->applyEditorOverlayKnobs();
}

EditorTransformGizmoState Renderer::editorTransformGizmoState() const noexcept
{
    return _impl ? _impl->editorTransformGizmo
                 : EditorTransformGizmoState{};
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

detail::RenderTargetPool* Renderer::renderTargetPool() noexcept
{
    return _impl ? &_impl->renderTargetPool : nullptr;
}

const detail::RenderTargetPool* Renderer::renderTargetPool() const noexcept
{
    return _impl ? &_impl->renderTargetPool : nullptr;
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
