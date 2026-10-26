#include "detail/GpuPointState.h"
#include "AYRenderer.h"
#include "AYRenderer/PortalSurfaceShaderSources.h"
#include "detail/RenderAssetBridge.h"
#include <AYResource/assetsImpl/Material.h>

#include "AYRenderer/F1DiagFlags.h"
#include "detail/BGFXAdapter.h"
#include "detail/BgfxMatrix.h"
#include "detail/CameraMath.h"
#include "detail/CameraViewsTestAccess.h"
#include "detail/ColorGradingPass.h"
#include "detail/ColorStylizationPass.h"
#include "detail/StylizedOutlinePass.h"
#include "detail/AnalogDisplayPass.h"
#include "detail/TemporalFeedbackPass.h"
#include "detail/BloomExtractPass.h"
#include "detail/BloomBlurPass.h"
#include "detail/BloomPipeline.h"
#include "detail/AutoExposurePass.h"
#include "detail/DepthHazePass.h"  // S4b (2026-07-23) — borrowed-ptr source for PassExecContext::depthHazePass + destroyResources.
#include "detail/DepthHazePipeline.h"
#include "detail/SsrPass.h"
#include "detail/VolumetricPass.h"
#include "detail/CloudPass.h"
#include "detail/DeferredDecalPass.h"
#include "detail/DeferredDecalVisibility.h"
#include "detail/DebugOverlay.h"
#include "detail/FgResource.h"        // §F2 (2026-07-24) — FrameGraph FgResourceId + FgTextureDesc
#include "detail/FrameDrawLists.h"
#include "detail/RigidInstancing.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FXAAPass.h"
#include "detail/SMAAPass.h"
#include "detail/TAAPass.h"
#include "detail/GBufferDebugPass.h"
#include "detail/GBufferPass.h"
#include "detail/FrameContext.h"
#include "detail/LightingPass.h"
#include "detail/MotionVectorPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/PostProcessPass.h"
#include "detail/PostProcessPipeline.h"
#include "detail/PreviewSceneValidation.h"
#include "detail/PresentPass.h"
#include "detail/EditorOverlayPass.h"
#include "detail/Forward2DOpaquePass.h"  // CM-1 (2026-08-11) — 2D lane pass factory.
#include "detail/RenderPassContract.h"
#include "detail/RenderPipeline.h"
#include "detail/RenderPipelineFactory.h"
#include "detail/RenderPipelineLifecycle.h"
#include "detail/RenderPipelinePolicy.h"
#include "detail/RenderTargetPool.h"
#include "detail/RenderViewOrder.h"
#include "detail/RenderViewNamespaces.h"
#include "detail/SSAOPass.h"        // §A1 SSAO MVP (2026-07-24) — SSAOPass factory + FrameGraph SSAOTexture resolve gate.
#include "detail/SSAOPipeline.h"
#include "detail/RenderResourceManager.h"
#include "detail/RenderResourceBlackboard.h"
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

namespace {

template<class Visitor>
void visitStylePasses(detail::RenderPipeline& pipeline,Visitor&& visitor)
{
    if(auto* p=pipeline.findPass<detail::ColorStylizationPass>())visitor(*p);
    if(auto* p=pipeline.findPass<detail::StylizedOutlinePass>())visitor(*p);
    if(auto* p=pipeline.findPass<detail::TemporalFeedbackPass>())visitor(*p);
    if(auto* p=pipeline.findPass<detail::AnalogDisplayPass>())visitor(*p);
}

} // namespace

struct Renderer::Impl {
    detail::BGFXAdapter           adapter;
    ayt::shader::ShaderResourcePool    shaderPool;
    std::shared_ptr<detail::GpuPointContext> gpuPoints;
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
    // matching `makeRenderPass` factory entries land in this PR.
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
    detail::RenderResourceBlackboard resourceBlackboard;
    detail::FrameDrawLists        frameDrawLists;
    struct CameraOutput {
        bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
        TextureHandle texture{};
        uint16_t width = 0, height = 0;
    };
    std::unordered_map<uint64_t, CameraOutput> cameraOutputs;
    detail::PostProcessPass previewPostProcess;
    bool cameraViewBatchSubmitted = false, cameraFrameOpen = false;
    struct RuntimeView {
        detail::ForwardOpaquePass opaque;
        detail::TransparentPass transparent;
    };
    std::unordered_map<uint32_t, std::unique_ptr<RuntimeView>> runtimeViews;
    bool                         frustumCullingEnabled = true;
    bool debugCapture=false;
    RenderDebugSnapshot debugSnapshot;
    bool                         rigidInstancingEnabled = true;

    detail::RenderResourceManager resources;
    detail::DebugOverlay          debugOverlay;
    InitDesc                      initDesc{};
    bool                          shaderPoolReady = false;
    uint32_t                      lastDrawCalls   = 0;
    uint32_t                      lastSceneItems  = 0;
    std::string                   pendingScreenshotBase;
    detail::ScreenshotCaptureQueue screenshotCaptures;

    ayt::math::Float4x4           mainView        = ayt::math::Float4x4::identity();
    ayt::math::Float4x4           mainProjection  = ayt::math::Float4x4::identity();
    bool                          mainCameraValid = false;
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
        if (detail::ShadowPass* shadow =
                pipeline.findPass<detail::ShadowPass>()) {
            shadow->setPcfEnabled(shadowPcfEnabled);
        }
    }

    // R5+ (Phase PostProcess) — per-host post-process knobs.
    // Defaults = no effect (bloom=0, exposure=1, ripple=0, tonemap=None).
    LensEffectsSettings            lensEffects;
    float                          postProcessBloomStrength  = 0.0f;
    float                          postProcessBloomThreshold = 1.0f;
    float                          postProcessBloomSoftKnee  = 0.5f;
    float                          postProcessExposure       = 1.0f;
    bool                           autoExposureEnabled       = false;
    float                          autoExposureKey           = 0.18f;
    float                          autoExposureMinimum       = 0.25f;
    float                          autoExposureMaximum       = 4.0f;
    float                          autoExposureBrightenSpeed = 2.0f;
    float                          autoExposureDarkenSpeed   = 1.0f;
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
    SsrSettings                    ssr;
    ScreenSpaceIndirectSettings    screenIndirect;
    VolumetricSettings             volumetric;
    CloudSettings                  clouds;
    VolumetricDensityBinding densityField;
    bool requestedVolumetricLocal=false;
    bool                          decalsEnabled = false;
    std::vector<DeferredDecal>     decals;
    float                          ssaoStrength = 0.0f;
    float                          ssaoRadius   = 0.5f;
    float                          ssaoBias     = 0.025f;

    // GBuffer attachment overlay knobs. Channel 3 is the packed material
    // scalar view; the public byte setter keeps the host surface lightweight.
    bool                           gbufferDebugEnabled  = false;
    uint8_t                        gbufferDebugChannel  = 0;
    bool                           wireframeEnabled = false;

    // EditorOverlay orientation widget. Stored on Impl so pipeline rebuilds
    // preserve the host's toggle even though the pass object is recreated.
    bool                           viewportOrientationAxisEnabled = false;
    EditorTransformGizmoState      editorTransformGizmo{};
    EditorGrid2DState              editorGrid2D{};
    std::vector<EditorDebugLine>    editorDebugLines;
    bool editorDebugDepthTest = false;
    EditorSelectionOutline2DState  editorSelectionOutline2D{};

    // Display-referred anti-aliasing is enabled by default. The state lives on
    // Impl so pipeline rebuilds preserve the host's runtime choice.
    bool                           fxaaEnabled = true;
    bool                           smaaEnabled = false;
    bool                           taaEnabled = false;
    uint8_t                        taaDebugView = 0;

    // Optional display-referred LUT grading. Disabled by default so all
    // pre-grading hosts retain a zero-allocation, byte-identical path.
    bool                           colorGradingEnabled = false;
    float                          colorGradingStrength = 0.75f;
    ColorGradingPreset             colorGradingPreset = ColorGradingPreset::Warm;

    ClearDesc sceneClear{};
    ColorStylizationSettings colorStylization{};

    void applyColorStylizationKnobs()
    {
        visitStylePasses(pipeline,[&](auto& pass){pass.setSettings(colorStylization);});
        if(auto* feedback=pipeline.findPass<detail::TemporalFeedbackPass>();feedback && !feedback->active())
            feedback->releaseHistory(adapter,&resourceBlackboard);
    }

    void applyEditorOverlayKnobs()
    {
        if(auto* pass=pipeline.findPass<detail::TransparentPass>()) pass->setDebugLines(editorDebugDepthTest?editorDebugLines:std::vector<EditorDebugLine>{});
        if (detail::EditorOverlayPass* editorOverlay =
                pipeline.findPass<detail::EditorOverlayPass>()) {
            editorOverlay->setOrientationAxisEnabled(
                viewportOrientationAxisEnabled);
            editorOverlay->setTransformGizmoState(editorTransformGizmo);
            editorOverlay->setGrid2DState(editorGrid2D);
            editorOverlay->setDebugLines(editorDebugDepthTest?std::vector<EditorDebugLine>{}:editorDebugLines);
            editorOverlay->setSelectionOutline2DState(editorSelectionOutline2D);
        }
    }

    void invalidateTemporalResources(
        detail::ResourceInvalidationReason reason)
    {
        resourceBlackboard.invalidateTemporal(reason);
        if(auto* feedback=pipeline.findPass<detail::TemporalFeedbackPass>())feedback->invalidateHistory(reason);
        if(auto* fog=pipeline.findPass<detail::VolumetricPass>())fog->invalidateHistory();
        if (detail::TAAPass* taa = pipeline.findPass<detail::TAAPass>()) {
            taa->invalidateHistory(reason);
        }
        if (detail::MotionVectorPass* motion =
                pipeline.findPass<detail::MotionVectorPass>()) {
            motion->invalidateHistory();
        }
        if (detail::AutoExposurePass* exposure =
                pipeline.findPass<detail::AutoExposurePass>()) {
            exposure->invalidateHistory(reason);
        }
    }

    void invalidateAaHistory(detail::ResourceInvalidationReason reason)
    {
        for(auto id:{detail::BlackboardResourceId::MotionVectors,detail::BlackboardResourceId::TaaHistoryRead,
            detail::BlackboardResourceId::TaaHistoryWrite})resourceBlackboard.invalidate(id,reason);
        if(auto* taa=pipeline.findPass<detail::TAAPass>())taa->invalidateHistory(reason);
        if(auto* motion=pipeline.findPass<detail::MotionVectorPass>())motion->invalidateHistory();
    }

    void applyAntiAliasingKnobs()
    {
        if (detail::FXAAPass* fxaa = pipeline.findPass<detail::FXAAPass>()) {
            fxaa->setEnabled(fxaaEnabled);
        }
        if (detail::SMAAPass* smaa = pipeline.findPass<detail::SMAAPass>()) {
            smaa->setEnabled(smaaEnabled);
        }
        if (detail::TAAPass* taa = pipeline.findPass<detail::TAAPass>()) {
            taa->setEnabled(taaEnabled);
            taa->setDebugView(taaDebugView);
            if (!taaEnabled) {
                invalidateAaHistory(
                    detail::ResourceInvalidationReason::FeatureDisabled);
            }
        }
    }

    void applyColorGradingKnobs()
    {
        if (detail::ColorGradingPass* grading =
                pipeline.findPass<detail::ColorGradingPass>()) {
            grading->setEnabled(colorGradingEnabled);
            grading->setStrength(colorGradingStrength);
            grading->setPreset(colorGradingPreset);
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
    PerformanceLighting performance;

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

    struct MainPresentationState {
        RenderPipelineDesc pipeline;
        bool overlayEnabled=false,overlaySuppressed=false;
        decltype(Impl::depthHazeEnabled) depthHazeEnabled;
        decltype(Impl::ssaoEnabled) ssaoEnabled;
        decltype(Impl::gbufferDebugEnabled) gbufferDebugEnabled;
        decltype(Impl::decalsEnabled) decalsEnabled;
        decltype(Impl::colorGradingEnabled) colorGradingEnabled;
        decltype(Impl::fxaaEnabled) fxaaEnabled;
        decltype(Impl::smaaEnabled) smaaEnabled;
        decltype(Impl::taaEnabled) taaEnabled;
        decltype(Impl::mainView) mainView;
        decltype(Impl::mainProjection) mainProjection;
        decltype(Impl::mainCameraValid) mainCameraValid;
        decltype(Impl::mainCameraPosition) mainCameraPosition;
        decltype(Impl::performance) performance;
        decltype(Impl::sceneLights) sceneLights;
        decltype(Impl::skySource) skySource;
        decltype(Impl::skyCubeTexture) skyCubeTexture;
        decltype(Impl::ambientStrength) ambientStrength;
        decltype(Impl::lensEffects) lensEffects;
        decltype(Impl::postProcessBloomStrength) postProcessBloomStrength;
        decltype(Impl::postProcessBloomThreshold) postProcessBloomThreshold;
        decltype(Impl::postProcessBloomSoftKnee) postProcessBloomSoftKnee;
        decltype(Impl::postProcessExposure) postProcessExposure;
        decltype(Impl::autoExposureEnabled) autoExposureEnabled;
        decltype(Impl::autoExposureKey) autoExposureKey;
        decltype(Impl::autoExposureMinimum) autoExposureMinimum;
        decltype(Impl::autoExposureMaximum) autoExposureMaximum;
        decltype(Impl::autoExposureBrightenSpeed) autoExposureBrightenSpeed;
        decltype(Impl::autoExposureDarkenSpeed) autoExposureDarkenSpeed;
        decltype(Impl::postProcessGamma) postProcessGamma;
        decltype(Impl::postProcessTonemapMode) postProcessTonemapMode;
        decltype(Impl::volumetric) volumetric;
        decltype(Impl::densityField) densityField;
        decltype(Impl::requestedVolumetricLocal) requestedVolumetricLocal;
        decltype(Impl::ssr) ssr;
        decltype(Impl::screenIndirect) screenIndirect;
        decltype(Impl::wireframeEnabled) wireframeEnabled;
        decltype(Impl::viewportOrientationAxisEnabled) viewportOrientationAxisEnabled;
        decltype(Impl::editorTransformGizmo) editorTransformGizmo;
        decltype(Impl::editorGrid2D) editorGrid2D;
        decltype(Impl::editorDebugLines) editorDebugLines;
        decltype(Impl::editorDebugDepthTest) editorDebugDepthTest;
        decltype(Impl::editorSelectionOutline2D) editorSelectionOutline2D;
        decltype(Impl::hasSimulationTime) hasSimulationTime;
        decltype(Impl::simulationTimeSeconds) simulationTimeSeconds;
    };
    std::unique_ptr<MainPresentationState> mainPresentationState;

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

    // Copied callbacks check this flag before touching the stable Impl.
    // Shutdown permanently disables the old registration before GPU teardown.
    // Registration, dispatch and destruction still require the submission
    // owner thread; an atomic flag is not a concurrent destruction fence.
    std::shared_ptr<std::atomic<bool>> aliveToken;
    uint64_t resourceContextId = 0;
    uint64_t resourceRefreshObserverToken = 0;

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
        // Each successful reinitialization receives a new live token.
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
    RenderPipelineDesc resolved = detail::resolveRenderPipelineDesc(desc);

    // R6-3b — reject malformed mount orders before tearing down the current
    // pipeline. This keeps a known-good pipeline live when a custom descriptor
    // has a duplicate slot or reads a resource before its required producer.
    const detail::RenderPipelineContractValidation contractValidation =
        detail::validateRenderPipelineContracts(resolved.passes);
    if (!contractValidation.valid()) {
        for (const detail::RenderPipelineContractError& error :
             contractValidation.errors) {
            const detail::RenderPassContract* contract =
                detail::renderPassContract(error.slot);
            std::fprintf(stderr,
                         "[RenderPipeline] descriptor rejected: "
                         "error=%s passIndex=%u pass=%s resource=%s; "
                         "keeping previous pipeline\n",
                         detail::renderPipelineContractErrorName(error.code),
                         static_cast<unsigned>(error.passIndex),
                         contract != nullptr
                             ? contract->name.data()
                             : "<unknown>",
                         error.code == detail::RenderPipelineContractErrorCode::
                                           MissingRequiredResource
                             || error.code == detail::RenderPipelineContractErrorCode::
                                                  OptionalProducerAfterConsumer
                             ? detail::renderPassResourceName(error.resource)
                             : "<none>");
        }
        return;
    }

    invalidateTemporalResources(
        detail::ResourceInvalidationReason::PipelineRebuild);
    resourceBlackboard.invalidateFrameOutputs(
        detail::ResourceInvalidationReason::PipelineRebuild);

    UIRenderBackend* retainedUi = nullptr;
    if (detail::UIPass* uiPass = pipeline.findPass<detail::UIPass>()) {
        retainedUi = uiPass->backend();
    }

    detail::destroyPipelineGpuResources(pipeline, adapter);

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
        if (auto pass = detail::makeRenderPass(slot)) {
            pipeline.addPass(slot, std::move(pass));
        }
    }
    pipelineDesc = std::move(resolved);

    if (retainedUi != nullptr) {
        if (detail::UIPass* uiPass = pipeline.findPass<detail::UIPass>()) {
            uiPass->setBackend(retainedUi);
        }
    }

    applyShadowQualityKnobs();
    applyAntiAliasingKnobs();
    applyColorGradingKnobs();
    applyColorStylizationKnobs();
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
Renderer& Renderer::operator=(Renderer&& other) noexcept
{
    if (this != &other) {
        shutdown();
        _impl = std::move(other._impl);
    }
    return *this;
}

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
    if (!_impl->shaderPoolReady && _impl->adapter.capsRendererType()!=bgfx::RendererType::Noop) {
        std::fprintf(stderr,"[Renderer] Shader startup failed; initialization cancelled.\n");
        shutdown();
        return false;
    }
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
    // A Renderer may be initialized again after shutdown(). Give the new
    // callback its own live token; callbacks copied before shutdown retain
    // the old, permanently-false token and remain safely disabled.
    if (!_impl->aliveToken
        || !_impl->aliveToken->load(std::memory_order_acquire)) {
        _impl->aliveToken = std::make_shared<std::atomic<bool>>(true);
    }
    auto aliveToken = _impl->aliveToken;
    static std::atomic<uint64_t> nextResourceContext{1};
    _impl->resourceContextId=nextResourceContext.fetch_add(1,std::memory_order_relaxed);
    // Impl has a stable address across Renderer moves. A callback must never
    // keep the moved-from public wrapper as its resource owner.
    Impl* owner = _impl.get();
    _impl->resourceRefreshObserverToken =
        ayt::resource::ResourceManager::instance().setRefreshObserver("Renderer",
        [owner, aliveToken](const std::string& path) {
            if (!aliveToken || !aliveToken->load(std::memory_order_acquire)) {
                return std::vector<ayt::resource::ResourceConsumerRefresh>{{
                    ayt::resource::ResourceRefreshState::NotHandled, {}, 0, 0,
                    "renderer is no longer active"}};
            }
            return owner->resources.onResourceFileChangedWithReport(path, owner->resourceContextId);
        });
    return true;
}

void Renderer::shutdown()
{
    if (!_impl) {
        return;
    }
    _impl->resourceContextId=0;
    // Disable any copied registration before the first GPU teardown. This
    // preserves owner-thread lifetime only; concurrent destruction is unsupported.
    if (_impl->aliveToken) _impl->aliveToken->store(false, std::memory_order_release);
    _impl->mainPresentationState.reset();
    _impl->densityField={};_impl->volumetric.local=_impl->requestedVolumetricLocal;
    for (auto& [id, view] : _impl->runtimeViews) {
        (void)id;
        if (_impl->adapter.isInitialized())
            view->transparent.destroyResources(_impl->adapter);
    }
    _impl->runtimeViews.clear();
    if (_impl->adapter.isInitialized())
        _impl->previewPostProcess.destroyResources(_impl->adapter);
    for (auto& [id, output] : _impl->cameraOutputs) {
        (void)id;
        _impl->resources.destroyTexture(output.texture);
        if (_impl->adapter.isInitialized()) _impl->adapter.destroy(output.framebuffer);
    }
    _impl->cameraOutputs.clear();

    _impl->invalidateTemporalResources(
        detail::ResourceInvalidationReason::Shutdown);

    // Pass instances survive shutdown/reinitialize. Clear GPU state while the
    // device and shader pool are alive, with attachment borrowers first.
    detail::destroyPipelineGpuResources(_impl->pipeline, _impl->adapter);

    // The scene target is owned here. Destroy it before Adapter shutdown;
    // numeric bgfx handles cannot identify a later device context.
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
    _impl->resourceBlackboard.clear(
        detail::ResourceInvalidationReason::Shutdown);
    _impl->renderTargetPool.shutdown();

    ayt::resource::ResourceManager::instance().clearRefreshObserver(_impl->resourceRefreshObserverToken);
    _impl->resourceRefreshObserverToken = 0;
    if(_impl->gpuPoints)_impl->gpuPoints->releaseBeforeDeviceShutdown();
    _impl->resources.shutdown();
    _impl->shaderPool.shutdown();
    _impl->adapter.shutdown();
    if(_impl->gpuPoints){_impl->gpuPoints->afterDeviceShutdown();_impl->gpuPoints.reset();}
    _impl->screenshotCaptures.advance(true);
    _impl->pendingScreenshotBase.clear();
    _impl->shaderPoolReady = false;
}

std::shared_ptr<detail::GpuPointState> Renderer::createGpuPointState(std::string* error) {
    if(!_impl||!_impl->adapter.isInitialized()){if(error)*error="Renderer is not initialized";return {};}
    if(!_impl->gpuPoints)_impl->gpuPoints=detail::GpuPointContext::create(_impl->adapter,_impl->aliveToken,error);
    return _impl->gpuPoints?_impl->gpuPoints->createState(error):nullptr;
}

bool Renderer::isInitialized() const noexcept
{
    return _impl && _impl->adapter.isInitialized();
}

uint64_t Renderer::resourceContextId() const noexcept
{
    return isInitialized()?_impl->resourceContextId:0;
}

void Renderer::beginFrame(const ClearDesc& clear)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }
    _impl->sceneClear = clear;
    auto finiteChannel = [](float value) {
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    };
    _impl->sceneClear.r = finiteChannel(clear.r);
    _impl->sceneClear.g = finiteChannel(clear.g);
    _impl->sceneClear.b = finiteChannel(clear.b);
    _impl->sceneClear.a = finiteChannel(clear.a);
    if(_impl->gpuPoints)_impl->gpuPoints->poll();
    _impl->cameraViewBatchSubmitted = false; _impl->cameraFrameOpen = true;
    _impl->compositeSceneViewId = -1;
    _impl->pipeline.resetFrameStats();
    _impl->debugOverlay.onBeginFrame();
    _impl->renderTargetPool.beginFrame();
    _impl->resourceBlackboard.beginFrame();
    _impl->adapter.beginFrame();
    _impl->adapter.setViewClear(detail::ForwardOpaquePass::kMainViewId, _impl->sceneClear);
}

void Renderer::beginCompositeFrame(const ClearDesc& clear, uint16_t fbWidth, uint16_t fbHeight)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }
    _impl->sceneClear = clear;
    auto finiteChannel = [](float value) {
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    };
    _impl->sceneClear.r = finiteChannel(clear.r);
    _impl->sceneClear.g = finiteChannel(clear.g);
    _impl->sceneClear.b = finiteChannel(clear.b);
    _impl->sceneClear.a = finiteChannel(clear.a);
    _impl->cameraViewBatchSubmitted = false; _impl->cameraFrameOpen = true;
    _impl->pipeline.resetFrameStats();
    _impl->debugOverlay.onBeginFrame();
    _impl->renderTargetPool.beginFrame();
    _impl->resourceBlackboard.beginFrame();

    // View 0 may still target the previous Forward scene FBO after a pipeline
    // switch. Composite chrome must always clear the full backbuffer.
    _impl->adapter.setViewFrameBuffer(0, BGFX_INVALID_HANDLE);
    // View 0: full-window clear only (never shrink this rect to the 3D hole).
    _impl->adapter.setViewRect(0, 0, 0, fbWidth, fbHeight);
    _impl->adapter.setViewClear(0, _impl->sceneClear);
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
    const auto* sceneLights = scene.sceneLights() ? scene.sceneLights() : _impl->sceneLights;
    // ShadowPass borrows this packet's snapshot only during the synchronous call.
    struct ClearBorrowedLights {
        detail::ShadowPass* pass;
        ~ClearBorrowedLights() { if (pass) pass->setSceneLightsRef(nullptr); }
    } clearBorrowedLights{_impl->pipeline.findPass<detail::ShadowPass>()};


    _impl->lastSceneItems = static_cast<uint32_t>(scene.items().size());
    _impl->lastDrawCalls  = 0;

    const bool performanceEnabled = lightingEnabled() && !_impl->performance.lights.empty();
    const SceneLights* effectiveLights = performanceEnabled ? &_impl->performance.shadowSources : sceneLights;
    _impl->performance.stats.transparentDraws = _impl->performance.stats.transparentBudgetSkipped = _impl->performance.stats.transparentUnsupportedMaterials = 0;
    _impl->performance.stats.surfaceDraws = _impl->performance.stats.volumeDraws = 0;
    _impl->performance.stats.culledLights = _impl->performance.stats.surfaceBudgetSkipped = _impl->performance.stats.volumeBudgetSkipped = 0;

    detail::TAAPass* taaPassPtr = nullptr;
    bool taaPrepared = false;
    if (detail::TAAPass* taa = _impl->pipeline.findPass<detail::TAAPass>()) {
        taaPassPtr = taa;
        // Current TAA reprojection requires the Deferred GBuffer. Forward
        // pipelines intentionally keep their stable, unjittered path.
        const auto* gbuffer = _impl->pipeline.findPass<detail::GBufferPass>();
        const auto* lighting = _impl->pipeline.findPass<detail::LightingPass>();
        const auto* post = _impl->pipeline.findPass<detail::PostProcessPass>();
        const auto* present = _impl->pipeline.findPass<detail::PresentPass>();
        // Match the final graph's resolve prerequisites before modifying the
        // scene projection. Mounting a disabled producer is not sufficient.
        const bool temporalChainEnabled = gbuffer && gbuffer->isEnabled()
            && lighting && lighting->isEnabled()
            && post && post->isEnabled() && present && present->isEnabled();
        if (temporalChainEnabled) {
            taaPrepared = taaPassPtr->prepareFrame(
                _impl->adapter,
                _impl->shaderPool,
                _impl->mainView,
                _impl->mainProjection,
                _impl->mainCameraPosition,
                _impl->viewportW,
                _impl->viewportH);
        }
        if (!taaPrepared) {
            const detail::ResourceInvalidationReason reason =
                taaPassPtr->isEnabled()
                    ? detail::ResourceInvalidationReason::PreparationFailed
                    : detail::ResourceInvalidationReason::FeatureDisabled;
            taaPassPtr->invalidateHistory(reason);
            _impl->resourceBlackboard.invalidateTemporal(reason);
        }
    }

    if (taaPassPtr != nullptr) {
        const detail::ResourceInvalidationReason historyReason =
            taaPassPtr->historyValid()
                ? detail::ResourceInvalidationReason::None
                : taaPassPtr->historyInvalidationReason();
        _impl->resourceBlackboard.publish(
            detail::BlackboardResourceId::TaaHistoryRead,
            detail::BlackboardResourceLifetime::PersistentHistory,
            taaPassPtr->readHistoryFbo(), BGFX_INVALID_HANDLE,
            _impl->viewportW, _impl->viewportH,
            taaPassPtr->historyGeneration(),
            taaPassPtr->historyValid(), historyReason);
        _impl->resourceBlackboard.publish(
            detail::BlackboardResourceId::TaaHistoryWrite,
            detail::BlackboardResourceLifetime::PersistentHistory,
            taaPassPtr->writeHistoryFbo(), BGFX_INVALID_HANDLE,
            _impl->viewportW, _impl->viewportH,
            taaPassPtr->historyGeneration(), false,
            detail::ResourceInvalidationReason::AwaitingProducer);
    }

    if (auto* forward = _impl->pipeline.findPass<detail::ForwardOpaquePass>()) {
        forward->setSceneClear(_impl->sceneClear);
    }
    if (auto* lighting = _impl->pipeline.findPass<detail::LightingPass>()) {
        lighting->setSceneClear(_impl->sceneClear);
    }

    detail::FrameContext frame;
    frame.view             = _impl->mainView;
    frame.projection       = taaPrepared
        ? taaPassPtr->jitteredProjection()
        : _impl->mainProjection;
    frame.cameraPosition   = _impl->mainCameraPosition;
    frame.collectDebugDraws=_impl->debugCapture;
    frame.frustumCulling = _impl->frustumCullingEnabled;
    frame.cullingViewProjection = _impl->mainProjection * _impl->mainView;
    frame.cullingHomogeneousDepth = _impl->adapter.capsHomogeneousDepth();
    // Two pixels cover the TAA sample footprint without changing each phase.
    frame.cullingMarginX = 4.0f / std::max<uint16_t>(_impl->viewportW, 1);
    frame.cullingMarginY = 4.0f / std::max<uint16_t>(_impl->viewportH, 1);
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
    frame.lensEffects = sanitizeLensEffects(_impl->lensEffects);
    frame.bloomStrength    = detail::sanitizeBloomStrength(
        _impl->postProcessBloomStrength);
    frame.exposure         = detail::sanitizePostProcessExposure(
        _impl->postProcessExposure);
    frame.gamma            = detail::sanitizePostProcessGamma(
        _impl->postProcessGamma);
    frame.tonemapMode      = _impl->postProcessTonemapMode;
    // §S4d — DepthHaze knobs (Editor / host). Defaults keep haze off.
    frame.hazeEnabled      = _impl->depthHazeEnabled;
    frame.ssr = _impl->ssr;
    frame.screenIndirect = _impl->screenIndirect;
    frame.volumetric = _impl->volumetric;
    frame.clouds = _impl->clouds;
    frame.densityField = _impl->densityField;
    frame.decalsEnabled = _impl->decalsEnabled;
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
    if (detail::GBufferPass* gbuffer =
            _impl->pipeline.findPass<detail::GBufferPass>()) {
        gbuffer->setGbufferSize(_impl->viewportW, _impl->viewportH);
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
        gbuffer->setPrevViewProj(_impl->prevMainView,
                                 _impl->prevMainProjection);
    }

    if (detail::MotionVectorPass* motion =
            _impl->pipeline.findPass<detail::MotionVectorPass>()) {
        motion->setOutputSize(_impl->viewportW, _impl->viewportH);
        // Motion vectors are a temporal dependency, not an always-on fifth
        // GBuffer MRT. Allocate/replay only when TAA resolved successfully.
        motion->setRequestedThisFrame(taaPrepared);
        const detail::TaaJitter jitter = taaPrepared
            ? taaPassPtr->currentJitter() : detail::TaaJitter{};
        motion->setProjectionJitter(
            _impl->viewportW ? jitter.x / _impl->viewportW : 0.0f,
            _impl->viewportH ? jitter.y / _impl->viewportH : 0.0f);
    }

    // §P5 B5 (2026-07-22) — broadcast viewport size to LightingPass
    // before dispatch so its execute() can ensure() the 1× RGBA16F
    // LightingOutput FBO at the correct W×H. Mirror the GBuffer
    // setGbufferSize block above (same viewport rect). Skipped
    // when Lighting isn't in the configured pipeline (Forward path)
    // — cutsheet §4.1 red line #4.
    if (detail::LightingPass* lighting =
            _impl->pipeline.findPass<detail::LightingPass>()) {
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
    if (detail::SkyboxPass* skybox =
            _impl->pipeline.findPass<detail::SkyboxPass>()) {
        skybox->setOutputSize(_impl->viewportW, _impl->viewportH);
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
    const detail::ShadowPass* shadowPassPtr =
        _impl->pipeline.findPass<detail::ShadowPass>();

    // §P5 B2 (2026-07-22) — when GBuffer is in the configured
    // pipeline, hand downstream passes (B5 LightingPass, future
    // B7+ multi-light consumers) a non-owning pointer so they can
    // read the GBuffer MRT attachments without FrameContext
    // writeback (§5.3 red line). Today the GBufferPass shell is
    // empty — gbufferFbo() returns BGFX_INVALID_HANDLE and the
    // shell's execute() Noop-gates — so consumers receive a
    // present-but-empty signal (same shape as ShadowPass on
    // Noop). Absent GBuffer ⇒ nullptr.
    detail::GBufferPass* gbufferPassPtr =
        _impl->pipeline.findPass<detail::GBufferPass>();
    if (gbufferPassPtr != nullptr) {
        gbufferPassPtr->resetFrameState();
        _impl->resourceBlackboard.publishGBuffer(
            gbufferPassPtr->gbufferFbo(),
            gbufferPassPtr->gbufferAlbedoRt(),
            gbufferPassPtr->gbufferNormalRt(),
            gbufferPassPtr->gbufferWorldPositionRt(),
            gbufferPassPtr->gbufferMaterialRt(),
            gbufferPassPtr->gbufferDepthRt(),
            gbufferPassPtr->gbufferWidth(),
            gbufferPassPtr->gbufferHeight(),
            gbufferPassPtr->targetGeneration(), false);
    }

    detail::MotionVectorPass* motionVectorPassPtr =
        _impl->pipeline.findPass<detail::MotionVectorPass>();
    if (motionVectorPassPtr != nullptr) {
        motionVectorPassPtr->resetFrameState();
        _impl->resourceBlackboard.publish(
            detail::BlackboardResourceId::MotionVectors,
            detail::BlackboardResourceLifetime::External,
            motionVectorPassPtr->velocityFbo(),
            motionVectorPassPtr->velocityTexture(),
            motionVectorPassPtr->width(), motionVectorPassPtr->height(),
            motionVectorPassPtr->targetGeneration(), false,
            detail::ResourceInvalidationReason::AwaitingProducer);
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
    detail::LightingPass* lightingPassPtr =
        _impl->pipeline.findPass<detail::LightingPass>();
    if (lightingPassPtr != nullptr) {
        lightingPassPtr->resetFrameState();
        const bgfx::FrameBufferHandle lightingFbo =
            lightingPassPtr->lightingOutputFbo();
        const bgfx::TextureHandle lightingTexture =
            detail::BGFXAdapter::isValid(lightingFbo)
                ? _impl->adapter.getFboAttachment(lightingFbo, 0)
                : bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _impl->resourceBlackboard.publish(
            detail::BlackboardResourceId::LightingColor,
            detail::BlackboardResourceLifetime::External,
            lightingFbo, lightingTexture,
            lightingPassPtr->lightingWidth(),
            lightingPassPtr->lightingHeight(),
            lightingPassPtr->targetGeneration(), false,
            detail::ResourceInvalidationReason::AwaitingProducer);
    }

    // §Skybox0 (2026-07-23) — borrowed pointer to the SkyboxPass in
    // the pipeline. nullptr when the host did not opt in via
    // `configurePipeline(makeDeferred())` (Forward / no skybox path).
    detail::SkyboxPass* skyboxPassPtr =
        _impl->pipeline.findPass<detail::SkyboxPass>();
    if (skyboxPassPtr != nullptr) {
        skyboxPassPtr->resetFrameState();
        _impl->resourceBlackboard.publish(
            detail::BlackboardResourceId::SkyboxColor,
            detail::BlackboardResourceLifetime::External,
            skyboxPassPtr->skyFbo(), skyboxPassPtr->skyRt(),
            skyboxPassPtr->skyWidth(), skyboxPassPtr->skyHeight(),
            skyboxPassPtr->targetGeneration(), false,
            detail::ResourceInvalidationReason::AwaitingProducer);
    }

    // Borrow both bloom passes and clear their production latches before graph
    // compilation. Downstream consumers require a successful submit from this
    // frame, never merely a still-valid attachment handle.
    detail::BloomExtractPass* bloomExtractPassPtr =
        _impl->pipeline.findPass<detail::BloomExtractPass>();
    if (bloomExtractPassPtr != nullptr) {
        bloomExtractPassPtr->resetFrameState();
    }

    detail::BloomBlurPass* bloomBlurPassPtr =
        _impl->pipeline.findPass<detail::BloomBlurPass>();
    if (bloomBlurPassPtr != nullptr) {
        bloomBlurPassPtr->resetFrameState();
    }

    detail::AutoExposurePass* autoExposurePassPtr =
        _impl->pipeline.findPass<detail::AutoExposurePass>();
    bool autoExposurePrepared = false;
    if (autoExposurePassPtr != nullptr) {
        autoExposurePassPtr->setSettings(
            _impl->autoExposureEnabled,
            _impl->autoExposureKey,
            _impl->autoExposureMinimum,
            _impl->autoExposureMaximum,
            _impl->autoExposureBrightenSpeed,
            _impl->autoExposureDarkenSpeed);
        autoExposurePrepared = autoExposurePassPtr->prepareFrame(
            _impl->adapter, _impl->viewportW, _impl->viewportH);
        const detail::ResourceInvalidationReason historyReason =
            autoExposurePassPtr->historyValid()
                ? detail::ResourceInvalidationReason::None
                : autoExposurePassPtr->invalidationReason();
        _impl->resourceBlackboard.publish(
            detail::BlackboardResourceId::AutoExposureRead,
            detail::BlackboardResourceLifetime::PersistentHistory,
            autoExposurePassPtr->readFbo(), BGFX_INVALID_HANDLE,
            1, 1, autoExposurePassPtr->generation(),
            autoExposurePassPtr->historyValid(), historyReason);
        _impl->resourceBlackboard.publish(
            detail::BlackboardResourceId::AutoExposureWrite,
            detail::BlackboardResourceLifetime::PersistentHistory,
            autoExposurePassPtr->writeFbo(), BGFX_INVALID_HANDLE,
            1, 1, autoExposurePassPtr->generation(), false,
            detail::ResourceInvalidationReason::AwaitingProducer);
    }

    // Borrow the haze producer so downstream passes can require a successful
    // submit from this frame before promoting FgSemantic::HazeSource.
    detail::DepthHazePass* depthHazePassPtr =
        _impl->pipeline.findPass<detail::DepthHazePass>();
    if (depthHazePassPtr != nullptr) {
        depthHazePassPtr->resetFrameState();
    }

    detail::SSAOPass* ssaoPassPtr =
        _impl->pipeline.findPass<detail::SSAOPass>();
    if (ssaoPassPtr != nullptr) {
        ssaoPassPtr->resetFrameState();
    }

    detail::PostProcessPass* postProcessPassPtr =
        _impl->pipeline.findPass<detail::PostProcessPass>();
    detail::FXAAPass* fxaaPassPtr =
        _impl->pipeline.findPass<detail::FXAAPass>();
    detail::SMAAPass* smaaPassPtr =
        _impl->pipeline.findPass<detail::SMAAPass>();
    detail::ColorGradingPass* colorGradingPassPtr =
        _impl->pipeline.findPass<detail::ColorGradingPass>();
    auto* colorStylizationPassPtr =
        _impl->pipeline.findPass<detail::ColorStylizationPass>();
    detail::PresentPass* presentPassPtr =
        _impl->pipeline.findPass<detail::PresentPass>();

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
    // When `sceneLights == nullptr` (host on Forward path /
    // never called setSceneLights), ShadowPass falls back to the
    // pre-C single key-light caster (pre-C byte-equivalent).
    if (detail::ShadowPass* shadow =
            _impl->pipeline.findPass<detail::ShadowPass>()) {
        shadow->setSceneLightsRef(effectiveLights,!performanceEnabled && scene.sceneLights()==nullptr);
    }

    // Scene projection may carry TAA jitter, but the post-Present editor
    // gizmo must remain pixel-stable. Keep this pass-local instead of growing
    // FrameContext, whose layout is an ODR/ABI diagnostic boundary.
    if (detail::EditorOverlayPass* overlay =
            _impl->pipeline.findPass<detail::EditorOverlayPass>()) {
        overlay->setUnjitteredProjection(_impl->mainProjection);
    }
    if (detail::TransparentPass* transparent =
            _impl->pipeline.findPass<detail::TransparentPass>()) {
        transparent->setSelectionUnjitteredProjection(_impl->mainProjection);
        const detail::TaaJitter selectionJitter = taaPrepared
            ? taaPassPtr->currentJitter()
            : detail::TaaJitter{};
        transparent->setSelectionProjectionJitter(selectionJitter.x,
                                                  selectionJitter.y);
    }

    // Decide stage eligibility from concrete runtime/pass capabilities here.
    // The pure plan below owns only resource declarations and dependencies.
    detail::FrameGraph& fg = _impl->frameGraph;
    const uint16_t fgW = _impl->viewportW;
    const uint16_t fgH = _impl->viewportH;
    // SceneColor is borrowed. Deferred uses Lighting output; Forward uses the
    // renderer-owned scene target.
    bgfx::FrameBufferHandle sceneColorHandle = sceneFbo;
    const detail::BlackboardResourceEntry* lightingColor =
        _impl->resourceBlackboard.find(
            detail::BlackboardResourceId::LightingColor);
    if (lightingPassPtr != nullptr && lightingColor != nullptr
        && detail::BGFXAdapter::isValid(lightingColor->framebuffer)) {
        sceneColorHandle = lightingColor->framebuffer;
    }
    const bool backendReady = _impl->adapter.isInitialized()
        && !_impl->adapter.isNoopBackend();
    const bool gbufferStagePresent = gbufferPassPtr != nullptr
        && gbufferPassPtr->isEnabled();
    const bool lightingStagePresent = lightingPassPtr != nullptr
        && lightingPassPtr->isEnabled();

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

    const bool hazePassEnabled = detail::selectDepthHazeStage(
        frame.hazeEnabled,
        frame.hazeStrength,
        depthHazePassPtr != nullptr,
        depthHazePassPtr != nullptr && depthHazePassPtr->isEnabled(),
        gbufferPassPtr != nullptr && gbufferPassPtr->isEnabled(),
        lightingPassPtr != nullptr && lightingPassPtr->isEnabled(),
        fgW,
        fgH) && backendReady;
    const detail::BloomStageState bloomStages = detail::selectBloomStages(
        lensEffectsActive(frame.lensEffects)?std::max(frame.bloomStrength,1.f):frame.bloomStrength,
        bloomExtractPassPtr != nullptr,
        bloomExtractPassPtr != nullptr && bloomExtractPassPtr->isEnabled(),
        bloomBlurPassPtr != nullptr,
        bloomBlurPassPtr != nullptr && bloomBlurPassPtr->isEnabled());
    const bool finalLdrStageEnabled = backendReady
        && fgW != 0 && fgH != 0
        && postProcessPassPtr != nullptr
        && postProcessPassPtr->isEnabled()
        && presentPassPtr != nullptr
        && presentPassPtr->isEnabled();
    const bool autoExposureStageEnabled = finalLdrStageEnabled
        && autoExposurePrepared
        && autoExposurePassPtr != nullptr
        && autoExposurePassPtr->isEnabled();
    const bool taaStageEnabled = finalLdrStageEnabled
        && taaPrepared && taaPassPtr != nullptr;
    const bool fxaaStageEnabled = finalLdrStageEnabled
        && fxaaPassPtr != nullptr && fxaaPassPtr->isEnabled();
    const bool smaaStageEnabled = finalLdrStageEnabled
        && smaaPassPtr != nullptr && smaaPassPtr->isEnabled();
    const bool colorGradingStageEnabled = finalLdrStageEnabled
        && colorGradingPassPtr != nullptr
        && colorGradingPassPtr->isEnabled()
        && colorGradingPassPtr->strength() > 0.0f
        && colorGradingPassPtr->preset() != ColorGradingPreset::Neutral;

    detail::PostProcessGraphPlanInput graphInput{};
    graphInput.width = fgW;
    graphInput.height = fgH;
    graphInput.sceneColor = sceneColorHandle;
    if (taaStageEnabled) {
        const detail::BlackboardResourceEntry* taaWrite =
            _impl->resourceBlackboard.find(
                detail::BlackboardResourceId::TaaHistoryWrite);
        const detail::BlackboardResourceEntry* taaRead =
            _impl->resourceBlackboard.find(
                detail::BlackboardResourceId::TaaHistoryRead);
        graphInput.taaWriteTarget = taaWrite != nullptr
            ? taaWrite->framebuffer
            : bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        graphInput.taaReadHistory = taaRead != nullptr
            ? taaRead->framebuffer
            : bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        const detail::BlackboardResourceEntry* gbuffer =
            _impl->resourceBlackboard.find(
                detail::BlackboardResourceId::GBufferAlbedo);
        graphInput.gbuffer = gbuffer != nullptr
            ? gbuffer->framebuffer
            : bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        const detail::BlackboardResourceEntry* motion =
            _impl->resourceBlackboard.find(
                detail::BlackboardResourceId::MotionVectors);
        graphInput.motionVectors = motion != nullptr
            ? motion->framebuffer
            : bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    graphInput.ssao = ssaoPassEnabled;
    const auto* ssrPass = _impl->pipeline.findPass<detail::SsrPass>();
    graphInput.ssr = ((frame.ssr.enabled && frame.ssr.strength > 0)
        || (frame.screenIndirect.enabled && frame.screenIndirect.strength > 0))
        && ssrPass && ssrPass->isEnabled() && gbufferPassPtr && lightingPassPtr;
    if (graphInput.ssr && !detail::BGFXAdapter::isValid(graphInput.gbuffer)) {
        graphInput.gbuffer = gbufferPassPtr->gbufferFbo();
    }
    graphInput.haze = hazePassEnabled;
    auto* decalPass = _impl->pipeline.findPass<detail::DeferredDecalPass>();
    const auto visibleDecals = backendReady && frame.decalsEnabled
        && decalPass && decalPass->isEnabled()
        ? detail::selectDeferredDecals(_impl->decals, frame)
        : detail::DeferredDecalVisibility{};
    if (decalPass && (!frame.decalsEnabled || _impl->decals.empty() || !decalPass->isEnabled())
        && detail::BGFXAdapter::isValid(decalPass->output(1)) && _impl->adapter.isInitialized())
        decalPass->destroyResources(_impl->adapter);
    if (backendReady && frame.decalsEnabled && visibleDecals.count != 0
        && decalPass && decalPass->isEnabled() && gbufferPassPtr && gbufferPassPtr->isEnabled()) {
        gbufferPassPtr->prepareOutput(_impl->adapter);
        if (decalPass->prepare(_impl->adapter, fgW, fgH, gbufferPassPtr->gbufferDepthRt(), gbufferPassPtr->targetGeneration())) {
            graphInput.decals = true;
            graphInput.baseGBuffer = gbufferPassPtr->gbufferFbo();
            graphInput.gbuffer = decalPass->output(visibleDecals.count);
        }
    }
    auto* fogPass = _impl->pipeline.findPass<detail::VolumetricPass>();
    graphInput.volumetric = backendReady && frame.volumetric.enabled && frame.volumetric.density > 0
        && fogPass && fogPass->isEnabled() && gbufferPassPtr && gbufferPassPtr->isEnabled()
        && lightingPassPtr && lightingPassPtr->isEnabled();
    graphInput.volumetricDownsample = frame.volumetric.downsample;
    graphInput.volumetricGrid = detail::runtimeVolumetricGridLayout(frame.volumetric,fgW,fgH);
    if(fogPass){
        const auto layout=graphInput.volumetric?graphInput.volumetricGrid:detail::VolumetricGridLayout{};
        fogPass->prepareHistory(_impl->adapter,frame.volumetric,layout);
        graphInput.volumetricHistoryRead=fogPass->historyRead();
        graphInput.volumetricHistoryWrite=fogPass->historyWrite();
    }
    if (graphInput.volumetric && !detail::BGFXAdapter::isValid(graphInput.gbuffer))
        graphInput.gbuffer = gbufferPassPtr->gbufferFbo();
    const auto* cloudPass = _impl->pipeline.findPass<detail::CloudPass>();
    graphInput.clouds = backendReady && (frame.clouds.atmosphereEnabled || (frame.clouds.enabled && frame.clouds.absorption > 0
        && ((frame.clouds.coverage > 0 && frame.clouds.density > 0) || (frame.clouds.highClouds.coverage > 0 && frame.clouds.highClouds.density > 0)))) && cloudPass && cloudPass->isEnabled()
        && gbufferPassPtr && gbufferPassPtr->isEnabled() && lightingPassPtr && lightingPassPtr->isEnabled();
    if (graphInput.clouds && !detail::BGFXAdapter::isValid(graphInput.gbuffer)) graphInput.gbuffer = gbufferPassPtr->gbufferFbo();
    graphInput.bloomExtract = bloomStages.extract;
    graphInput.bloomBlur = bloomStages.blur;
    graphInput.autoExposure = autoExposureStageEnabled;
    if (autoExposureStageEnabled) {
        graphInput.autoExposureHistory = autoExposurePassPtr->readFbo();
        graphInput.autoExposureTarget = autoExposurePassPtr->writeFbo();
    }
    graphInput.finalLdr = finalLdrStageEnabled;
    graphInput.motionVector = taaStageEnabled
        && motionVectorPassPtr != nullptr
        && motionVectorPassPtr->isEnabled();
    graphInput.taa = taaStageEnabled;
    graphInput.fxaa = fxaaStageEnabled;
    graphInput.smaa = smaaStageEnabled;
    graphInput.colorGrading = colorGradingStageEnabled;
    graphInput.colorStylization = finalLdrStageEnabled && colorStylizationPassPtr
        && colorStylizationPassPtr->isEnabled() && colorStylizationPassPtr->active();
    auto* feedbackPass=_impl->pipeline.findPass<detail::TemporalFeedbackPass>();
    if(feedbackPass && feedbackPass->isEnabled() && feedbackPass->active()) {
        if(finalLdrStageEnabled) {
            graphInput.temporalFeedback=feedbackPass->prepareFrame(_impl->adapter,_impl->shaderPool,frame,
                _impl->viewportX,_impl->viewportY,_impl->viewportW,_impl->viewportH,&_impl->resourceBlackboard);
            if(graphInput.temporalFeedback) {
                graphInput.feedbackReadHistory=feedbackPass->readFbo();
                graphInput.feedbackWriteTarget=feedbackPass->writeFbo();
            }
        } else {
            feedbackPass->invalidateHistory(detail::ResourceInvalidationReason::ProducerFailure);
            _impl->resourceBlackboard.invalidate(detail::BlackboardResourceId::FeedbackHistoryRead,detail::ResourceInvalidationReason::ProducerFailure);
            _impl->resourceBlackboard.invalidate(detail::BlackboardResourceId::FeedbackHistoryWrite,detail::ResourceInvalidationReason::ProducerFailure);
        }
    }
    const auto* displayPass=_impl->pipeline.findPass<detail::AnalogDisplayPass>();
    graphInput.analogDisplay=finalLdrStageEnabled && displayPass && displayPass->isEnabled() && displayPass->active();
    auto* outlinePassPtr=_impl->pipeline.findPass<detail::StylizedOutlinePass>();
    graphInput.stylizedOutline=finalLdrStageEnabled && outlinePassPtr
        && outlinePassPtr->isEnabled() && outlinePassPtr->active();
    graphInput.stylizationDepth = graphInput.stylizedOutline
        && _impl->colorStylization.outlineMode >= StylizationOutlineMode::Depth
        && _impl->colorStylization.outlineStrength>0;
    graphInput.stylizationNormals = graphInput.stylizationDepth
        && _impl->colorStylization.outlineMode == StylizationOutlineMode::DepthNormal
        && gbufferPassPtr && gbufferPassPtr->isEnabled();
    if(graphInput.stylizationDepth && gbufferPassPtr && gbufferPassPtr->isEnabled()
        && !detail::BGFXAdapter::isValid(graphInput.gbuffer))
        graphInput.gbuffer=gbufferPassPtr->gbufferFbo();
    graphInput.smaaIntermediatePointSampled =
        detail::SMAAPass::kIntermediatePointSampled;

    const detail::PostProcessGraphPlanResult graphPlan =
        detail::buildPostProcessGraphPlan(fg, graphInput);

    detail::buildFrameDrawLists(
        scene, _impl->resources.meshes(), _impl->resources.materials(),
        frame, _impl->frameDrawLists);

    // GBufferDebug now overlays the selected attachment directly into the
    // game viewport on view 250. No hidden host-owned FBO is allocated; UI
    // view 255 still renders afterwards, so Editor chrome remains intact.

    if (!graphPlan.compileSucceeded) {
        std::fprintf(stderr, "[FrameGraph] compile failed: %zu structural error(s)\n",
                     fg.compileErrors().size());
        for (const detail::FgCompileError& error : fg.compileErrors()) {
            std::fprintf(stderr,
                         "[FrameGraph]   pass=%u resource=%u reason=%s\n",
                         static_cast<unsigned>(error.passIndex),
                         static_cast<unsigned>(error.resource),
                         detail::fgCompileErrorName(error.code));
        }
    }

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
        effectiveLights,
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
        effectiveLights,
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
        motionVectorPassPtr,
        _impl->wireframeEnabled,
        &_impl->frameDrawLists,
        &_impl->resourceBlackboard,
    };
    detail::InstancingStats instancingStats{};
    ctx.rigidInstancing = _impl->rigidInstancingEnabled;
    ctx.instancingStats = &instancingStats;
    ctx.decals = &_impl->decals;
    ctx.decalVisibility = &visibleDecals;
    ctx.performanceLights = performanceEnabled ? &_impl->performance : nullptr;

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

    // FrameGraph eligibility gates the pipeline's registered passes.
    // Opaque geometry writes depth first; transparency reuses that depth
    // with DEPTH_TEST_LEQUAL but does not WRITE_Z. Transparent submission is
    // stable back-to-front: explicit DrawItem::sortKey first, camera distance
    // second, with Sequential view mode preserving that CPU order.
    // The main LDR chain runs AA, outline, grading, artistic color, temporal
    // feedback, display simulation and Present. Each successful stage promotes
    // PresentSource; Present samples it into the Game View rect. UI runs after
    // these image effects. UIPass ignores the viewId arg and
    // delegates to its injected UIRenderBackend (see UIPass.h for the
    // chrome lifecycle contract — execute() DOES call flushBatches;
    // beginFrame/endFrame stay on the host's UIManager::render lambda).
    //
    // Shadow mounting follows the pipeline descriptor; setShadowsEnabled
    // controls an installed pass. Its depth-only target and submit gate
    // honor the same backend/Noop ownership rules as other passes.
    // ShadowPass::execute Noop-gates
    // cleanly when the adapter is uninitialized or Noop.
    detail::configureRenderViewOrder(_impl->adapter);
    _impl->lastDrawCalls = _impl->pipeline.executeAll(ctx);
    if(feedbackPass)feedbackPass->finishFrame(&_impl->resourceBlackboard);
    if (_impl->resourceBlackboard.findProduced(detail::BlackboardResourceId::VolumetricColor)) {
        if (const auto* volume=_impl->pipeline.findPass<detail::VolumetricPass>())
            _impl->debugOverlay.setVolumetricStats(volume->stats());
    }
    // Commit temporal state as a pair. A skipped/failed resolve must not
    // leave color at t-2 while motion advances to t-1; a failed motion pass
    // must likewise never reuse its old object snapshots on the next frame.
    if (taaPassPtr != nullptr) {
        taaPassPtr->finishFrame(
            _impl->pipeline.findPass<detail::MotionVectorPass>(),
            &_impl->resourceBlackboard);
    }

    // R6-6 diagnostics: publish one backend-neutral architecture snapshot
    // after every producer has had a chance to update the blackboard. This is
    // observational only and does not participate in graph execution.
    const detail::FgCompileStats& fgStats = fg.stats();
    const detail::FgRetentionStats retention = fg.retentionStats();
    RenderGraphFrameStats graphStats{};
    graphStats.compiled = true;
    graphStats.compileSucceeded = graphPlan.compileSucceeded;
    graphStats.compileErrors = static_cast<uint16_t>(std::min<size_t>(
        fg.compileErrors().size(), UINT16_MAX));
    graphStats.declaredPasses = fgStats.declaredPasses;
    graphStats.livePasses = fgStats.livePasses;
    graphStats.logicalResources = fgStats.logicalResources;
    graphStats.transientTargets = fgStats.physicalTargets;
    graphStats.retainedTargets = retention.leasedTargets;
    graphStats.idleResources = retention.idleResources;
    graphStats.releasedTargets = retention.releasedTargets;

    const detail::BlackboardDiagnosticStats blackboardStats =
        _impl->resourceBlackboard.diagnosticStats();
    RenderResourceFrameStats resourceStats{};
    resourceStats.declaredResources = blackboardStats.declaredResources;
    resourceStats.availableResources = blackboardStats.availableResources;
    resourceStats.validResources = blackboardStats.validResources;
    resourceStats.producedResources = blackboardStats.producedResources;
    resourceStats.invalidResources = blackboardStats.invalidResources;
    resourceStats.persistentHistoryResources =
        blackboardStats.persistentHistoryResources;
    _impl->debugOverlay.setArchitectureStats(graphStats, resourceStats);
    if(_impl->debugCapture) {
        auto& snapshot=_impl->debugSnapshot;++snapshot.frame;
        snapshot.draws=_impl->frameDrawLists.debugDraws;snapshot.dropped=_impl->frameDrawLists.debugDropped;
        snapshot.lights=sceneLights?*sceneLights:SceneLights{};
        snapshot.lights.count=std::min(snapshot.lights.count,kMaxSceneLights);
        if(!sceneLights) snapshot.lights.add(Light::directional(_impl->directionalLightDir,_impl->directionalLightColor));
        snapshot.homogeneousDepth=_impl->adapter.capsHomogeneousDepth();snapshot.shadowsEnabled=shadowsEnabled();
        snapshot.shadowViewProjections.clear();
        if(auto* shadow=_impl->pipeline.findPass<detail::ShadowPass>();shadow && shadow->hasSampleableShadow()) {
            const auto* values=shadow->atlasLightViewProjsColumnMajor();
            for(uint32_t slot=0;slot<std::min(shadow->shadowProjectionCount(),8u);++slot) {
                auto matrix=ayt::math::Float4x4::identity();
                for(int row=0;row<4;++row)for(int col=0;col<4;++col) matrix(row,col)=values[slot*16+col*4+row];
                snapshot.shadowViewProjections.push_back(matrix);
            }
        }
    }
    const auto& visibility = _impl->frameDrawLists.stats;
    _impl->debugOverlay.setVisibilityStats(visibility.frustumTested,
        visibility.frustumCulled, visibility.frustumBypassed);
    _impl->debugOverlay.setInstancingStats(instancingStats.draws, instancingStats.objects);

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

bool Renderer::renderPreviewScene(const RenderScene &scene, const PreviewSceneCamera &camera,
                                  uint16_t framebuffer, uint16_t width, uint16_t height, uint8_t firstView,
                                  uint32_t clearRgba, const PreviewSceneSettings &requestedSettings,
                                  uint16_t hdrFramebuffer)
{
    if (!isInitialized() || !width || !height || !detail::validPreviewCamera(camera))
        return false;
    const auto settings = detail::sanitizePreviewSettings(requestedSettings);
    detail::configureRenderViewOrder(_impl->adapter);
    detail::FrameContext frame;
    frame.view = camera.view;
    frame.projection = camera.projection;
    frame.cameraPosition = camera.position;
    frame.lightDirection = settings.lightDirection;
    frame.lightColor = settings.lightColor;
    frame.exposure = settings.exposure;
    frame.gamma = settings.gamma;
    frame.tonemapMode = static_cast<detail::FrameContext::TonemapMode>(settings.toneMap);
    detail::FrameDrawLists lists;
    detail::buildFrameDrawLists(scene, _impl->resources.meshes(), _impl->resources.materials(), frame, lists);
    detail::PassExecContext ctx{
        _impl->adapter,
        _impl->shaderPool,
        scene,
        _impl->resources.meshes(),
        _impl->resources.textures(),
        _impl->resources.materials(),
        0,
        0,
        width,
        height,
        frame,
        static_cast<uint8_t>(firstView + 1u),
        bgfx::FrameBufferHandle{settings.displayTransform ? hdrFramebuffer : framebuffer}};
    ctx.drawLists = &lists;
    ctx.sceneLights = settings.lights ? settings.lights : scene.sceneLights();
    detail::ForwardOpaquePass opaque;
    opaque.execute(ctx);
    _impl->adapter.setViewClearRaw(ctx.viewId, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clearRgba, 1.0f, 0);
    ctx.viewId = static_cast<uint8_t>(firstView + 2u);
    detail::TransparentPass transparent;
    transparent.execute(ctx);
    detail::Forward2DOpaquePass overlay(static_cast<uint8_t>(firstView + 3u));
    overlay.execute(ctx);
    if (settings.displayTransform)
        return _impl->previewPostProcess.executeToTarget(ctx, static_cast<uint8_t>(firstView + 4u),
                                                         bgfx::FrameBufferHandle{framebuffer}) == 1;
    return true;
}

CameraOutputHandle Renderer::createCameraOutput(uint16_t width, uint16_t height) {
    if (!isInitialized() || !width || !height) return {};
    const auto framebuffer = _impl->adapter.createFrameBuffer(width,height,bgfx::TextureFormat::RGBA8,true);
    if (!bgfx::isValid(framebuffer)) return {};
    auto texture = _impl->resources.registerBorrowedTexture(
        _impl->adapter.getFboAttachment(framebuffer,0),width,height);
    if (!texture.isValid()) { _impl->adapter.destroy(framebuffer); return {}; }
    static std::atomic<uint64_t> nextOutputId{1};
    const auto id = nextOutputId.fetch_add(1,std::memory_order_relaxed);
    _impl->cameraOutputs.emplace(id,Impl::CameraOutput{framebuffer,texture,width,height});
    return {id};
}
TextureHandle Renderer::cameraOutputTexture(CameraOutputHandle output) const noexcept {
    if (!_impl || !output.id) return {};
    const auto found = _impl->cameraOutputs.find(output.id);
    return found != _impl->cameraOutputs.end() ? found->second.texture : TextureHandle{};
}
MaterialHandle Renderer::createPortalSurfaceMaterial(CameraOutputHandle output) {
    const auto texture = cameraOutputTexture(output);
    if (!texture.isValid()) return {};
    auto material = createMaterialFromBgfxSc(kPortalSurfaceVertexSc,
        kPortalSurfaceFragmentSc,kPortalSurfaceVaryingSc);
    if (!material.isValid()) return {};
    const bool bottomLeft = bgfx::getCaps() && bgfx::getCaps()->originBottomLeft;
    setMaterialColor(material,"portalUv",0.5f,bottomLeft ? 0.5f : -0.5f,0.5f,0.5f);
    setMaterialTexture(material,"portalColor",texture);
    return material;
}
bool Renderer::cameraOutputMatches(CameraOutputHandle output, uint16_t width, uint16_t height) const noexcept {
    if (!_impl) return false;
    const auto found = _impl->cameraOutputs.find(output.id);
    return found != _impl->cameraOutputs.end() && found->second.width == width && found->second.height == height;
}
uint16_t detail::CameraViewsTestAccess::framebuffer(const Renderer& renderer, CameraOutputHandle output) noexcept {
    if (!renderer._impl) return UINT16_MAX;
    const auto found = renderer._impl->cameraOutputs.find(output.id);
    return found != renderer._impl->cameraOutputs.end() ? found->second.framebuffer.idx : UINT16_MAX;
}
uint16_t detail::CameraViewsTestAccess::finalColor(Renderer& renderer) noexcept {
    if(!renderer._impl||!renderer.isInitialized())return UINT16_MAX;
    const auto fbo=renderer._impl->frameGraph.resolve(detail::FgResourceId::FinalLdrColor);
    return renderer._impl->adapter.getFboAttachment(fbo,0).idx;
}
detail::BGFXAdapter* detail::CameraViewsTestAccess::adapter(Renderer& renderer) noexcept {
    return renderer.bgfxAdapter();
}
void Renderer::destroyCameraOutput(CameraOutputHandle output) {
    if (!_impl) return;
    const auto found = _impl->cameraOutputs.find(output.id);
    if (found == _impl->cameraOutputs.end()) return;
    _impl->resources.destroyTexture(found->second.texture);
    if (_impl->adapter.isInitialized()) _impl->adapter.destroy(found->second.framebuffer);
    _impl->cameraOutputs.erase(found);
}

bool Renderer::renderViews(std::span<const CameraRenderView> views) {
    if (!isInitialized() || !_impl->cameraFrameOpen || _impl->cameraViewBatchSubmitted
        || views.empty() || views.size() > kMaximumCameraViews) return false;
    auto finiteMatrix = [](const math::Float4x4& m) {
        for (int i = 0; i < 16; ++i) if (!std::isfinite(m.ptr()[i])) return false;
        return true;
    };
    // Validate every recipe before touching any GPU view. The range 64..111 is reserved
    // for these three-pass runtime chains; editor/particle/main IDs remain unchanged.
    for (size_t i = 0; i < views.size(); ++i) {
        const auto& view = views[i];
        if (!view.scene || !view.width || !view.height || !finiteMatrix(view.view) || !finiteMatrix(view.projection)
            || !std::isfinite(view.position.x) || !std::isfinite(view.position.y) || !std::isfinite(view.position.z)) return false;
        if (view.transparent2DOutput && (!view.output.id || view.clearRgba != 0)) return false;
        for (size_t previous = 0; previous < i; ++previous)
            if (views[previous].id == view.id || (view.output.id && views[previous].output.id == view.output.id)) return false;
        if (view.output.id) {
            const auto found = _impl->cameraOutputs.find(view.output.id);
            if (found == _impl->cameraOutputs.end() || found->second.width != view.width
                || found->second.height != view.height || view.x || view.y) return false;
        } else if (uint32_t(view.x) + view.width > _impl->initDesc.width
            || uint32_t(view.y) + view.height > _impl->initDesc.height) return false;
        // WorldLit2D requires the deferred material lane; reject it explicitly in this Forward API.
        for (const auto& item : view.scene->items()) {
            if (view.transparent2DOutput &&
                (!item.payload || item.particleBatch ||
                 item.payload->renderDomain != RenderDomain2D::SceneOverlay)) return false;
            if (view.spaceRegionId && item.spaceRegionId != view.spaceRegionId) continue;
            if (!item.payload && !(item.cameraLayerMask & view.layerMask)) continue;
            if (item.payload && item.payload->renderDomain == RenderDomain2D::WorldLit) return false;
            const auto material = _impl->resources.materials().find(item.material.id);
            if (material == _impl->resources.materials().end()) continue;
            for (const auto& slot : material->second.textures)
                for (size_t producer = i; producer < views.size(); ++producer) {
                    const auto target = _impl->cameraOutputs.find(views[producer].output.id);
                    if (target != _impl->cameraOutputs.end() && target->second.texture.id == slot.texture.id) return false;
                }
        }
    }
    _impl->cameraViewBatchSubmitted = true;
    detail::configureRenderViewOrder(_impl->adapter);
    for (size_t i = 0; i < views.size(); ++i) {
        const auto& view = views[i];
        auto& state = _impl->runtimeViews[view.id];
        if (!state) state = std::make_unique<Impl::RuntimeView>();
        RenderScene visible;
        copyCameraVisibleScene(*view.scene,visible,view.layerMask,true,view.spaceRegionId);
        detail::FrameContext frame;
        frame.view = view.view; frame.projection = view.projection; frame.cameraPosition = view.position;
        frame.lightDirection = _impl->directionalLightDir.normalize(); frame.lightColor = _impl->directionalLightColor;
        frame.frustumCulling = _impl->frustumCullingEnabled;
        detail::FrameDrawLists lists;
        detail::buildFrameDrawLists(visible,_impl->resources.meshes(),_impl->resources.materials(),frame,lists);
        const uint8_t firstView = uint8_t(detail::kCameraFirstView + i * 3);
        const auto fbo = view.output.id ? _impl->cameraOutputs.at(view.output.id).framebuffer
            : bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        detail::PassExecContext ctx{_impl->adapter,_impl->shaderPool,visible,
            _impl->resources.meshes(),_impl->resources.textures(),_impl->resources.materials(),
            view.x,view.y,view.width,view.height,frame,firstView,fbo};
        ctx.drawLists = &lists;
        ctx.transparent2DOutput = view.transparent2DOutput;
        state->opaque.execute(ctx);
        _impl->adapter.setViewFrameBuffer(firstView,fbo);
        _impl->adapter.setViewRect(firstView,view.output.id?0:view.x,view.output.id?0:view.y,view.width,view.height);
        _impl->adapter.setViewClearRaw(firstView,BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,view.clearRgba,1.f,0);
        // Empty opaque scenes still execute the camera's requested clear.
        _impl->adapter.touch(firstView);
        ctx.viewId = uint8_t(firstView + 1);
        state->transparent.execute(ctx);
        detail::Forward2DOpaquePass overlay(uint8_t(firstView + 2));
        overlay.execute(ctx);
    }
    // Remove detached viewport resources while the device is still alive.
    for (auto it = _impl->runtimeViews.begin(); it != _impl->runtimeViews.end();) {
        if (std::none_of(views.begin(),views.end(),[&](const auto& view) { return view.id == it->first; })) {
            it->second->transparent.destroyResources(_impl->adapter); it = _impl->runtimeViews.erase(it);
        } else ++it;
    }
    return true;
}

void Renderer::setDebugCaptureEnabled(bool enabled) { if(_impl) { _impl->debugCapture=enabled;if(!enabled) _impl->debugSnapshot={}; } }
RenderDebugSnapshot Renderer::debugSnapshot() const { return _impl?_impl->debugSnapshot:RenderDebugSnapshot{}; }

void Renderer::setFrustumCullingEnabled(bool enabled)
{
    if (_impl) _impl->frustumCullingEnabled = enabled;
}

void Renderer::setRigidInstancingEnabled(bool enabled)
{
    if (_impl) _impl->rigidInstancingEnabled = enabled;
}

bool Renderer::rigidInstancingEnabled() const noexcept
{
    return _impl && _impl->rigidInstancingEnabled;
}

bool Renderer::frustumCullingEnabled() const noexcept
{
    return _impl && _impl->frustumCullingEnabled;
}

void Renderer::resize(uint32_t width, uint32_t height)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }

    _impl->invalidateTemporalResources(
        detail::ResourceInvalidationReason::Resize);
    _impl->resourceBlackboard.invalidateFrameOutputs(
        detail::ResourceInvalidationReason::Resize);

    // Borrowed framebuffer shells must retire before their scene/GBuffer/
    // Lighting producers. A later render/prepare is not the reset boundary.
    detail::destroyMainAttachmentResources(_impl->pipeline, _impl->adapter);
    if (detail::BGFXAdapter::isValid(_impl->sceneFbo)) {
        _impl->adapter.destroy(_impl->sceneFbo);
        _impl->sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    _impl->sceneFboW = 0;
    _impl->sceneFboH = 0;
    if (auto* taa = _impl->pipeline.findPass<detail::TAAPass>()) {
        taa->destroyResources(_impl->adapter);
    }
    // FrameGraph returns its leases before the pool invalidates retained
    // UI/other leases, and both complete before the device reset below.
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

    _impl->cameraFrameOpen = false;
    if (!_impl->pendingScreenshotBase.empty()) {
        const auto capture=_impl->screenshotCaptures.enqueue(_impl->pendingScreenshotBase);
        if(!capture.empty())_impl->adapter.requestScreenshot(capture);
        _impl->pendingScreenshotBase.clear();
    }

    _impl->debugOverlay.onEndFrame(_impl->lastDrawCalls, _impl->lastSceneItems,
                                   _impl->pipeline.lastPassStats(),
                                   _impl->viewportX, _impl->viewportY,
                                   _impl->viewportW, _impl->viewportH);
    _impl->adapter.endFrame();
    if(_impl->gpuPoints)_impl->gpuPoints->poll();
    _impl->debugOverlay.onFrameSubmitted();
    _impl->compositeSceneViewId = -1;

    _impl->screenshotCaptures.advance();
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

std::shared_ptr<const MeshSubmissionInfo> Renderer::meshSubmissionInfo(MeshHandle mesh) const noexcept
{
    if(!isInitialized())return {};
    const auto& meshes=_impl->resources.meshes();
    const auto found=meshes.find(mesh.id);
    return found==meshes.end()?nullptr:found->second.submissionInfo;
}

bool Renderer::isMaterialResident(MaterialHandle material) const noexcept
{
    return isInitialized()&&_impl->resources.materials().contains(material.id);
}

MeshHandle Renderer::createMesh32(const void* vertices, uint32_t vertexCount,
                                  const VertexLayoutDesc& layout, const uint32_t* indices,
                                  uint32_t indexCount, bool buildWireframe)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.createMesh32(vertices, vertexCount, layout, indices, indexCount, buildWireframe);
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

MaterialHandle Renderer::createMaterialFromResource(const ayt::resource::Material &material,
                                                    const std::string &ownerPath)
{
    if (!_impl || !_impl->adapter.isInitialized() || !_impl->shaderPoolReady)
        return {};
    return detail::bindMaterialFromResource(_impl->resources, material, ownerPath);
}

MaterialHandle Renderer::createMaterial2D(const Material2DDesc& desc,
                                          const std::string& cacheKey)
{
    if (!_impl || !_impl->adapter.isInitialized() || !_impl->shaderPoolReady) {
        return {};
    }
    return _impl->resources.createMaterial2D(desc, cacheKey);
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

TextureHandle Renderer::loadTexture(const std::string& path, bool srgb)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return {};
    }
    return _impl->resources.loadTexture(path, srgb);
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

void Renderer::setMaterialPbrSurfaceSettings(MaterialHandle material, const PbrSurfaceSettings &settings)
{
    if (_impl)
        _impl->resources.setMaterialPbrSurfaceSettings(material, settings);
}
PbrSurfaceSettings Renderer::materialPbrSurfaceSettings(MaterialHandle material) const
{
    return _impl ? _impl->resources.materialPbrSurfaceSettings(material) : PbrSurfaceSettings{};
}

void Renderer::setMaterialVec3(MaterialHandle material, const char *uniformName, float x, float y, float z)
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
    _impl->mainCameraValid = true;
}

void Renderer::setMainCamera(const ayt::math::Float4x4& view,
                             const ayt::math::Float4x4& projection,
                             const ayt::math::FVector3& position)
{
    if (!_impl) return;
    _impl->mainCameraPosition = position;
    setMainCamera(view, projection);
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

bool Renderer::setPerformanceLights(std::span<const PerformanceLight> lights, PerformanceLightBudget budget, std::string* error)
{
    if(!_impl)return false;
    PerformanceLighting next;
    if(!next.assign(lights,budget,error))return false;
    for(const auto& light:next.lights)if(light.pattern==LightPattern::Texture){
        const auto found=_impl->resources.textures().find(light.goboTexture.id);
        if(found==_impl->resources.textures().end()
            || !detail::BGFXAdapter::isValid(found->second.handle)){
            if(error)*error="Texture Gobo must be a live Renderer-owned texture";
            return false;
        }
    }
    for(const auto& light:next.lights)if(light.iesProfile){
        const auto found=_impl->resources.textures().find(light.iesTexture.id);
        if(found==_impl->resources.textures().end()||!detail::BGFXAdapter::isValid(found->second.handle)){if(error)*error="IES atlas must be a live Renderer-owned texture";return false;}
    }
    _impl->performance=std::move(next);
    if(error)error->clear();
    return true;
}
PerformanceLightStats Renderer::performanceLightStats() const noexcept
{
    return _impl ? _impl->performance.stats : PerformanceLightStats{};
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
        _impl->invalidateTemporalResources(
            detail::ResourceInvalidationReason::BackendReset);
        _impl->resourceBlackboard.invalidateFrameOutputs(
            detail::ResourceInvalidationReason::BackendReset);
        detail::destroyMainAttachmentResources(_impl->pipeline, _impl->adapter);
        if (detail::BGFXAdapter::isValid(_impl->sceneFbo)) {
            _impl->adapter.destroy(_impl->sceneFbo);
            _impl->sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        }
        _impl->sceneFboW = 0;
        _impl->sceneFboH = 0;
        // Preserve the existing MSAA reset policy for post-process geometry
        // and exposure history; the full pipeline remains mounted.
        if (_impl->adapter.isInitialized()) {
            if (auto* pass = _impl->pipeline.findPass<detail::BloomExtractPass>())
                pass->destroyResources(_impl->adapter);
            if (auto* pass = _impl->pipeline.findPass<detail::BloomBlurPass>())
                pass->destroyResources(_impl->adapter);
            if (auto* pass = _impl->pipeline.findPass<detail::AutoExposurePass>())
                pass->destroyResources(_impl->adapter);
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

void Renderer::setLensEffects(const LensEffectsSettings& settings){if(_impl)_impl->lensEffects=sanitizeLensEffects(settings);}
LensEffectsSettings Renderer::lensEffects() const noexcept {return _impl?_impl->lensEffects:LensEffectsSettings{};}
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

void Renderer::setAutoExposureEnabled(bool enabled)
{
    if (_impl) {
        _impl->autoExposureEnabled = enabled;
        if (!enabled) {
            if (detail::AutoExposurePass* pass =
                    _impl->pipeline.findPass<detail::AutoExposurePass>()) {
                pass->invalidateHistory(
                    detail::ResourceInvalidationReason::FeatureDisabled);
            }
        }
    }
}

bool Renderer::autoExposureEnabled() const noexcept
{
    return _impl && _impl->autoExposureEnabled;
}

void Renderer::setAutoExposureKey(float keyValue)
{
    if (_impl) {
        _impl->autoExposureKey = std::clamp(
            std::isfinite(keyValue) ? keyValue : 0.18f, 0.01f, 2.0f);
    }
}

void Renderer::setAutoExposureRange(float minimum, float maximum)
{
    if (!_impl) return;
    minimum = std::clamp(std::isfinite(minimum) ? minimum : 0.25f,
                         0.01f, 16.0f);
    maximum = std::clamp(std::isfinite(maximum) ? maximum : 4.0f,
                         minimum, 32.0f);
    _impl->autoExposureMinimum = minimum;
    _impl->autoExposureMaximum = maximum;
}

void Renderer::setAutoExposureAdaptation(float brightenSpeed,
                                         float darkenSpeed)
{
    if (!_impl) return;
    _impl->autoExposureBrightenSpeed = std::clamp(
        std::isfinite(brightenSpeed) ? brightenSpeed : 2.0f,
        0.01f, 20.0f);
    _impl->autoExposureDarkenSpeed = std::clamp(
        std::isfinite(darkenSpeed) ? darkenSpeed : 1.0f,
        0.01f, 20.0f);
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
    if (!_impl || _impl->fxaaEnabled == enabled) {
        return;
    }
    _impl->fxaaEnabled = enabled;
    if (enabled) {
        _impl->smaaEnabled = false;
        _impl->taaEnabled = false;
    }
    _impl->applyAntiAliasingKnobs();
}

bool Renderer::fxaaEnabled() const noexcept
{
    return _impl != nullptr && _impl->fxaaEnabled;
}

void Renderer::setSmaaEnabled(bool enabled)
{
    if (!_impl || _impl->smaaEnabled == enabled) {
        return;
    }
    _impl->smaaEnabled = enabled;
    if (enabled) {
        _impl->fxaaEnabled = false;
        _impl->taaEnabled = false;
    }
    _impl->applyAntiAliasingKnobs();
}

bool Renderer::smaaEnabled() const noexcept
{
    return _impl != nullptr && _impl->smaaEnabled;
}

void Renderer::setTaaEnabled(bool enabled)
{
    if (!_impl || _impl->taaEnabled == enabled) {
        return;
    }
    _impl->taaEnabled = enabled;
    if (enabled) {
        _impl->fxaaEnabled = false;
        _impl->smaaEnabled = false;
    }
    _impl->invalidateAaHistory(
        enabled ? detail::ResourceInvalidationReason::Manual
                : detail::ResourceInvalidationReason::FeatureDisabled);
    _impl->applyAntiAliasingKnobs();
}

bool Renderer::taaEnabled() const noexcept
{
    return _impl != nullptr && _impl->taaEnabled;
}

void Renderer::notifyCameraCut()
{
    if (_impl) _impl->invalidateTemporalResources(
        detail::ResourceInvalidationReason::CameraCut);
}

void Renderer::setTaaDebugView(uint8_t mode)
{
    if (!_impl) return;
    _impl->taaDebugView = mode <= 5 ? mode : 0;
    if (auto* taa = _impl->pipeline.findPass<detail::TAAPass>()) {
        taa->setDebugView(_impl->taaDebugView);
    }
}

uint8_t Renderer::taaDebugView() const noexcept
{
    return _impl ? _impl->taaDebugView : 0;
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

// SSR host settings survive pipeline replacement, but never store GPU handles.
void Renderer::beginMainPresentation() {
    if (!_impl || _impl->mainPresentationState) return;
    auto saved=std::make_unique<Impl::MainPresentationState>();
    saved->pipeline=_impl->pipelineDesc;
    saved->overlayEnabled=_impl->debugOverlay.isEnabled();
    saved->overlaySuppressed=_impl->debugOverlay.isSuppressed();
    saved->depthHazeEnabled=_impl->depthHazeEnabled;
    saved->ssaoEnabled=_impl->ssaoEnabled;
    saved->gbufferDebugEnabled=_impl->gbufferDebugEnabled;
    saved->decalsEnabled=_impl->decalsEnabled;
    saved->colorGradingEnabled=_impl->colorGradingEnabled;
    saved->fxaaEnabled=_impl->fxaaEnabled;
    saved->smaaEnabled=_impl->smaaEnabled;
    saved->taaEnabled=_impl->taaEnabled;
    saved->mainView=_impl->mainView;
    saved->mainProjection=_impl->mainProjection;
    saved->mainCameraValid=_impl->mainCameraValid;
    saved->mainCameraPosition=_impl->mainCameraPosition;
    saved->performance=_impl->performance;
    saved->sceneLights=_impl->sceneLights;
    saved->skySource=_impl->skySource;
    saved->skyCubeTexture=_impl->skyCubeTexture;
    saved->ambientStrength=_impl->ambientStrength;
    saved->lensEffects=_impl->lensEffects;
    saved->postProcessBloomStrength=_impl->postProcessBloomStrength;
    saved->postProcessBloomThreshold=_impl->postProcessBloomThreshold;
    saved->postProcessBloomSoftKnee=_impl->postProcessBloomSoftKnee;
    saved->postProcessExposure=_impl->postProcessExposure;
    saved->autoExposureEnabled=_impl->autoExposureEnabled;
    saved->autoExposureKey=_impl->autoExposureKey;
    saved->autoExposureMinimum=_impl->autoExposureMinimum;
    saved->autoExposureMaximum=_impl->autoExposureMaximum;
    saved->autoExposureBrightenSpeed=_impl->autoExposureBrightenSpeed;
    saved->autoExposureDarkenSpeed=_impl->autoExposureDarkenSpeed;
    saved->postProcessGamma=_impl->postProcessGamma;
    saved->postProcessTonemapMode=_impl->postProcessTonemapMode;
    saved->volumetric=_impl->volumetric;
    saved->densityField=_impl->densityField;
    saved->requestedVolumetricLocal=_impl->requestedVolumetricLocal;
    saved->ssr=_impl->ssr;
    saved->screenIndirect=_impl->screenIndirect;
    saved->wireframeEnabled=_impl->wireframeEnabled;
    saved->viewportOrientationAxisEnabled=_impl->viewportOrientationAxisEnabled;
    saved->editorTransformGizmo=_impl->editorTransformGizmo;
    saved->editorGrid2D=_impl->editorGrid2D;
    saved->editorDebugLines=_impl->editorDebugLines;
    saved->editorDebugDepthTest=_impl->editorDebugDepthTest;
    saved->editorSelectionOutline2D=_impl->editorSelectionOutline2D;
    saved->hasSimulationTime=_impl->hasSimulationTime;
    saved->simulationTimeSeconds=_impl->simulationTimeSeconds;
    _impl->mainPresentationState=std::move(saved);
    _impl->invalidateTemporalResources(detail::ResourceInvalidationReason::FeatureDisabled);
}
void Renderer::endMainPresentation() {
    if (!_impl || !_impl->mainPresentationState) return;
    auto saved=std::move(_impl->mainPresentationState);
    _impl->depthHazeEnabled=saved->depthHazeEnabled;
    _impl->ssaoEnabled=saved->ssaoEnabled;
    _impl->gbufferDebugEnabled=saved->gbufferDebugEnabled;
    _impl->decalsEnabled=saved->decalsEnabled;
    _impl->colorGradingEnabled=saved->colorGradingEnabled;
    _impl->fxaaEnabled=saved->fxaaEnabled;
    _impl->smaaEnabled=saved->smaaEnabled;
    _impl->taaEnabled=saved->taaEnabled;
    _impl->mainView=std::move(saved->mainView);
    _impl->mainProjection=std::move(saved->mainProjection);
    _impl->mainCameraValid=std::move(saved->mainCameraValid);
    _impl->mainCameraPosition=std::move(saved->mainCameraPosition);
    _impl->performance=std::move(saved->performance);
    _impl->sceneLights=std::move(saved->sceneLights);
    _impl->skySource=std::move(saved->skySource);
    _impl->skyCubeTexture=std::move(saved->skyCubeTexture);
    _impl->ambientStrength=std::move(saved->ambientStrength);
    _impl->lensEffects=std::move(saved->lensEffects);
    _impl->postProcessBloomStrength=std::move(saved->postProcessBloomStrength);
    _impl->postProcessBloomThreshold=std::move(saved->postProcessBloomThreshold);
    _impl->postProcessBloomSoftKnee=std::move(saved->postProcessBloomSoftKnee);
    _impl->postProcessExposure=std::move(saved->postProcessExposure);
    _impl->autoExposureEnabled=std::move(saved->autoExposureEnabled);
    _impl->autoExposureKey=std::move(saved->autoExposureKey);
    _impl->autoExposureMinimum=std::move(saved->autoExposureMinimum);
    _impl->autoExposureMaximum=std::move(saved->autoExposureMaximum);
    _impl->autoExposureBrightenSpeed=std::move(saved->autoExposureBrightenSpeed);
    _impl->autoExposureDarkenSpeed=std::move(saved->autoExposureDarkenSpeed);
    _impl->postProcessGamma=std::move(saved->postProcessGamma);
    _impl->postProcessTonemapMode=std::move(saved->postProcessTonemapMode);
    _impl->volumetric=std::move(saved->volumetric);
    _impl->densityField=std::move(saved->densityField);
    _impl->requestedVolumetricLocal=std::move(saved->requestedVolumetricLocal);
    _impl->ssr=std::move(saved->ssr);
    _impl->screenIndirect=std::move(saved->screenIndirect);
    _impl->wireframeEnabled=std::move(saved->wireframeEnabled);
    _impl->viewportOrientationAxisEnabled=std::move(saved->viewportOrientationAxisEnabled);
    _impl->editorTransformGizmo=std::move(saved->editorTransformGizmo);
    _impl->editorGrid2D=std::move(saved->editorGrid2D);
    _impl->editorDebugLines=std::move(saved->editorDebugLines);
    _impl->editorDebugDepthTest=std::move(saved->editorDebugDepthTest);
    _impl->editorSelectionOutline2D=std::move(saved->editorSelectionOutline2D);
    _impl->hasSimulationTime=std::move(saved->hasSimulationTime);
    _impl->simulationTimeSeconds=std::move(saved->simulationTimeSeconds);
    _impl->debugOverlay.setEnabled(saved->overlayEnabled);
    _impl->debugOverlay.setSuppressed(saved->overlaySuppressed);
    _impl->applyPipelineDesc(saved->pipeline);
    setSkySourceCube(_impl->skyCubeTexture);
    _impl->applyEditorOverlayKnobs();
    _impl->invalidateTemporalResources(detail::ResourceInvalidationReason::FeatureDisabled);
}

void Renderer::setVolumetricSettings(const VolumetricSettings& settings) {
    // Independent of decal authoring.
    if (_impl){auto value=sanitizeVolumetricSettings(settings);_impl->requestedVolumetricLocal=value.local;if(_impl->densityField.profile)value.local=true;_impl->volumetric=value;}
}
bool Renderer::setVolumetricDensityField(VolumetricDensityBinding binding,std::string* error){
    if(!_impl)return false;
    if(bool(binding.profile)!=binding.texture.isValid()){if(error)*error="Density grid requires a matching profile and atlas";return false;}
    if(binding.profile){const auto found=_impl->resources.textures().find(binding.texture.id);if(found==_impl->resources.textures().end()||!detail::BGFXAdapter::isValid(found->second.handle)){if(error)*error="Density atlas must be live and Renderer-owned";return false;}}
    if(binding.profile!=_impl->densityField.profile||binding.texture.id!=_impl->densityField.texture.id)resetVolumetricHistory();
    _impl->densityField=std::move(binding);_impl->volumetric.local=_impl->densityField.profile?true:_impl->requestedVolumetricLocal;if(error)error->clear();return true;
}
VolumetricDensityBinding Renderer::volumetricDensityField() const noexcept {return _impl?_impl->densityField:VolumetricDensityBinding{};}
void Renderer::resetVolumetricHistory() {
    if(_impl)if(auto* pass=_impl->pipeline.findPass<detail::VolumetricPass>())pass->invalidateHistory();
}
VolumetricSettings Renderer::volumetricSettings() const noexcept {
    return _impl ? _impl->volumetric : VolumetricSettings{};
}
void Renderer::setCloudSettings(const CloudSettings& settings) { if (_impl) _impl->clouds = sanitizeCloudSettings(settings); }
CloudSettings Renderer::cloudSettings() const noexcept { return _impl ? _impl->clouds : CloudSettings{}; }
void Renderer::setDeferredDecals(const std::vector<DeferredDecal>& decals) {
    if (!_impl) return;
    std::vector<DeferredDecal> copy;
    for (const auto& input : decals) {
        if (copy.size() == kMaxDeferredDecals) break;
        auto d = sanitizeDeferredDecal(input);
        if (d.opacity > 0) copy.push_back(d);
    }
    _impl->decals = std::move(copy);
}
const std::vector<DeferredDecal>& Renderer::deferredDecals() const noexcept {
    static const std::vector<DeferredDecal> empty;
    return _impl ? _impl->decals : empty;
}
void Renderer::setDeferredDecalsEnabled(bool enabled) { if (_impl) _impl->decalsEnabled = enabled; }
bool Renderer::deferredDecalsEnabled() const noexcept { return _impl && _impl->decalsEnabled; }
void Renderer::setSsrSettings(const SsrSettings& settings) {
    if (_impl) _impl->ssr = sanitizeSsrSettings(settings);
}
SsrSettings Renderer::ssrSettings() const noexcept {
    return _impl ? _impl->ssr : SsrSettings{};
}

void Renderer::setScreenSpaceIndirectSettings(const ScreenSpaceIndirectSettings& settings) {
    if (_impl) _impl->screenIndirect = sanitizeScreenSpaceIndirect(settings);
}
ScreenSpaceIndirectSettings Renderer::screenSpaceIndirectSettings() const noexcept {
    return _impl ? _impl->screenIndirect : ScreenSpaceIndirectSettings{};
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

bool Renderer::mainCameraMatrices(
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProjection) const noexcept
{
    if (!_impl || !_impl->mainCameraValid) {
        return false;
    }
    outView = _impl->mainView;
    outProjection = _impl->mainProjection;
    return true;
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
    if(_impl->densityField.profile&&_impl->densityField.texture.id==texture.id){_impl->densityField={};_impl->volumetric.local=_impl->requestedVolumetricLocal;resetVolumetricHistory();}
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

void Renderer::setEditorGrid2DState(const EditorGrid2DState& state)
{
    if (!_impl) return;
    _impl->editorGrid2D = state;
    _impl->applyEditorOverlayKnobs();
}

void Renderer::setEditorDebugDepthTest(bool enabled) { if(!_impl) return; _impl->editorDebugDepthTest=enabled; _impl->applyEditorOverlayKnobs(); }
bool Renderer::editorDebugDepthTest() const noexcept { return _impl && _impl->editorDebugDepthTest; }
bool Renderer::setEditorDebugLines(const std::vector<EditorDebugLine>& lines)
{
    if (!_impl || lines.size() > kMaxEditorDebugLines) return false;
    for (const auto& l : lines) {
        for (float v : {l.from.x,l.from.y,l.from.z,l.to.x,l.to.y,l.to.z})
            if (!std::isfinite(v)) return false;
    }
    _impl->editorDebugLines = lines;
    _impl->applyEditorOverlayKnobs();
    return true;
}
size_t Renderer::editorDebugLineCount() const noexcept
{
    return _impl ? _impl->editorDebugLines.size() : 0;
}

EditorGrid2DState Renderer::editorGrid2DState() const noexcept
{
    return _impl ? _impl->editorGrid2D : EditorGrid2DState{};
}

void Renderer::setEditorSelectionOutline2DState(
    const EditorSelectionOutline2DState& state)
{
    if (!_impl) return;
    _impl->editorSelectionOutline2D = state;
    _impl->applyEditorOverlayKnobs();
}

EditorSelectionOutline2DState
Renderer::editorSelectionOutline2DState() const noexcept
{
    return _impl ? _impl->editorSelectionOutline2D
                 : EditorSelectionOutline2DState{};
}

/// Enable the text HUD and backend per-view GPU timers together. Suppression hides
/// only text; profiled RenderFrameStats remain available. Default disabled.
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

void Renderer::setWireframeEnabled(bool enabled)
{
    if (_impl) _impl->wireframeEnabled = enabled;
}

bool Renderer::isWireframeEnabled() const noexcept
{
    return _impl && _impl->wireframeEnabled;
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

ResourceResidencySnapshot Renderer::resourceResidencySnapshot(uint32_t maxTextureRows) const
{
    if(!isInitialized())return {};
    return _impl->resources.resourceResidencySnapshot(_impl->resourceContextId,maxTextureRows);
}

bool Renderer::captureScreenshot(const std::string& filePath)
{
    if (!_impl || !_impl->adapter.isInitialized() || filePath.empty()) {
        return false;
    }
    if (_impl->initDesc.backend == Backend::Noop || _impl->initDesc.windowHandle == nullptr) {
        return false;
    }
    if(_impl->screenshotCaptures.full())return false;
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

void Renderer::setColorStylizationSettings(const ColorStylizationSettings& settings)
{
    if (!_impl) return;
    _impl->colorStylization = sanitizeColorStylization(settings);
    _impl->applyColorStylizationKnobs();
}
void Renderer::resetStylizationHistory()
{
    if(!_impl)return;
    if(auto* pass=_impl->pipeline.findPass<detail::TemporalFeedbackPass>())
        pass->invalidateHistory(detail::ResourceInvalidationReason::Manual);
    _impl->resourceBlackboard.invalidate(detail::BlackboardResourceId::FeedbackHistoryRead,detail::ResourceInvalidationReason::Manual);
    _impl->resourceBlackboard.invalidate(detail::BlackboardResourceId::FeedbackHistoryWrite,detail::ResourceInvalidationReason::Manual);
}
ColorStylizationSettings Renderer::colorStylizationSettings() const noexcept
{
    return _impl ? _impl->colorStylization : ColorStylizationSettings{};
}
} // namespace ayt::render
