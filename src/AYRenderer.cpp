#include "AYRenderer.h"

#include "AYF1DiagFlags.h"
#include "detail/BGFXAdapter.h"
#include "detail/BgfxMatrix.h"
#include "detail/DebugOverlay.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPipeline.h"
#include "detail/RenderResourceManager.h"
#include "detail/ScreenshotSidecar.h"
#include "detail/ShaderPoolSetup.h"
#include "detail/ShadowPass.h"
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

namespace ayt::render
{

std::size_t detailDiagSizeofFrameContext()
{
    return sizeof(detail::FrameContext);
}

RenderPipelineDesc RenderPipelineDesc::makeDefault()
{
    return RenderPipelineDesc{{
        RenderPassSlot::ForwardOpaque,
        RenderPassSlot::Transparent,
        RenderPassSlot::PostProcess,
        RenderPassSlot::UI,
    }};
}

RenderPipelineDesc RenderPipelineDesc::makeForwardWithShadows()
{
    return RenderPipelineDesc{{
        RenderPassSlot::Shadow,
        RenderPassSlot::ForwardOpaque,
        RenderPassSlot::Transparent,
        RenderPassSlot::PostProcess,
        RenderPassSlot::UI,
    }};
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
    }
    return nullptr;
}

} // namespace

struct Renderer::Impl {
    detail::BGFXAdapter           adapter;
    ayt::shader::ShaderResourcePool    shaderPool;
    // Product default = RenderPipelineDesc::makeDefault()
    // (FO → Transparent → PostProcess → UI). Shadow is opt-in via
    // Renderer::configurePipeline(makeForwardWithShadows()) — hosts
    // such as AYEditor assemble the ordered slot list; the Renderer
    // rebuilds unique_ptr passes from that desc. GBuffer / Lighting
    // slots are not implemented yet.
    detail::RenderPipeline        pipeline;
    RenderPipelineDesc            pipelineDesc = RenderPipelineDesc::makeDefault();

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

    // R5+ (Phase PostProcess, 2026-07-20) — per-host post-process knobs.
    // Defaults = no effect (bloomStrength=0 disables bloom; exposure=1
    // is neutral; tonemap=None passes through). Renderer::render()
    // copies these into FrameContext before dispatching the pipeline
    // so PostProcessPass::execute sees them in `frame.*`.
    float                          postProcessBloomStrength = 0.0f;
    float                          postProcessExposure      = 1.0f;
    detail::FrameContext::TonemapMode postProcessTonemapMode = detail::FrameContext::TonemapMode::None;

    // P0 (2026-07-20) — wall-clock origin for FrameContext.timeSeconds.
    // std::chrono::steady_clock is monotonic (immune to wall-clock
    // adjustments) which is what R5+ post-process effects (time-of-day
    // color grading, bloom pulse) need. Set on the first successful
    // initialize(); consumed by Renderer::render into frame.timeSeconds.
    std::chrono::steady_clock::time_point renderClockOrigin{};

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

#if AY_F1_DIAG_FRAME_SHADOW
    // F1 diag — cache written into FrameContext each render().
    bgfx::FrameBufferHandle lastFrameShadowFbo =
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
#endif

    Impl()
        : resources(adapter, shaderPool)
    {
#if AY_F1_DIAG_DEFAULT_SHADOW
        applyPipelineDesc(RenderPipelineDesc::makeForwardWithShadows());
#else
        applyPipelineDesc(RenderPipelineDesc::makeDefault());
#endif
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

    pipeline.clear();
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
    if (_impl->renderClockOrigin.time_since_epoch().count() != 0) {
        const auto now = std::chrono::steady_clock::now();
        const std::chrono::duration<float> elapsed = now - _impl->renderClockOrigin;
        frame.timeSeconds = elapsed.count();
    }
    // R5+ — host-configured post-process knobs. Rendered every frame
    // even when the value hasn't changed because the FrameContext is
    // stack-local; cost is negligible (3 floats + 1 byte enum).
    frame.bloomStrength = _impl->postProcessBloomStrength;
    frame.exposure      = _impl->postProcessExposure;
    frame.tonemapMode   = _impl->postProcessTonemapMode;

#if AY_F1_DIAG_FRAME_SHADOW
    frame.shadowFboIdx  = _impl->lastFrameShadowFbo.idx;
    frame.lightViewProj = ayt::math::Float4x4::identity();
    frame.lightIndex    = 0;
#if AY_F1_DIAG_LIGHT
    if (!scene.lights().empty()) {
        frame.lightDirection = scene.lights()[0].direction;
        frame.lightColor     = scene.lights()[0].color;
    }
#endif
#endif

    const uint8_t viewId = _impl->compositeSceneViewId >= 0
                               ? static_cast<uint8_t>(_impl->compositeSceneViewId)
                               : detail::ForwardOpaquePass::kMainViewId;

    _impl->lastDrawCalls = 0;

    // P2 scene FBO — used by fullscreen / non-composite hosts so
    // PostProcess can sample the scene. Editor composite mode draws
    // 3D straight into the backbuffer panel hole instead: the
    // FBO→blit path was leaving the Game View black (view-id / blit
    // coupling). INVALID ⇒ FO/Transparent use (vx,vy,vw,vh) on the
    // default backbuffer (pre-P2 editor behavior).
    const bgfx::FrameBufferHandle sceneFbo =
        (_impl->compositeSceneViewId >= 0)
            ? bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE}
            : _impl->ensureSceneFbo();

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
    // Per-pass isEnabled() guards are honored by the pipeline; today
    // all four default to true (set by the RenderPass base ctor).
    _impl->lastDrawCalls = _impl->pipeline.executeAll(ctx);

#if AY_F1_DIAG_FRAME_SHADOW && AY_F1_DIAG_DEFAULT_SHADOW
    for (auto& pass : _impl->pipeline.passes()) {
        if (pass && pass->name() == "Shadow") {
            auto* shadow = static_cast<detail::ShadowPass*>(pass.get());
            _impl->lastFrameShadowFbo = shadow->shadowFbo();
            break;
        }
    }
#endif
}

void Renderer::resize(uint32_t width, uint32_t height)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }

    _impl->initDesc.width  = width;
    _impl->initDesc.height = height;
    _impl->adapter.resetResolution(width, height, _impl->initDesc.vsync);

    // P2 (PR-D) — invalidate the scene FBO so render()'s ensureSceneFbo
    // path rebuilds it at the new size on the next frame. We don't
    // destroy eagerly here because:
    //   (a) bgfx::reset may transiently drop view attachments; the FBO
    //       handle table gets re-validated on the next bgfx::frame().
    //   (b) ensureSceneFbo is gated on isInitialized() and will detect
    //       that _sceneFboW != width and recycle.
    _impl->sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    _impl->sceneFboW = 0;
    _impl->sceneFboH = 0;
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
    sceneFbo = adapter.createFrameBuffer(static_cast<uint16_t>(w),
                                         static_cast<uint16_t>(h),
                                         bgfx::TextureFormat::RGBA8,
                                         /*withDepth=*/true);
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
        _impl->sceneFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _impl->sceneFboW = 0;
        _impl->sceneFboH = 0;
        if (detail::RenderPass* shadowPass = _impl->pipeline.findPass("Shadow")) {
            if (_impl->adapter.isInitialized()) {
                static_cast<detail::ShadowPass*>(shadowPass)
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
