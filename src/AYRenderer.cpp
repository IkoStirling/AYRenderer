#include "AYRenderer.h"

#include "detail/BGFXAdapter.h"
#include "detail/DebugOverlay.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FrameContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPipeline.h"
#include "detail/RenderResourceManager.h"
#include "detail/ScreenshotSidecar.h"
#include "detail/ShaderPoolSetup.h"
#include "detail/TransparentPass.h"
#include "detail/UiGpuContext.h"
#include "detail/UIPass.h"
#include "AYUIRenderBackend.h"

#include "AYShaderResourcePool.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <chrono>
#include <cstring>

namespace ayt::render
{

struct Renderer::Impl {
    detail::BGFXAdapter           adapter;
    ayt::shader::ShaderResourcePool    shaderPool;
    // U1+ — RenderPipeline owns the ordered list of RenderPass
    // subclasses. P0 (2026-07-20) inserts PostProcessPass between
    // Transparent and UI to match design.md:467-470 kFullPipelineOrder:
    //   kFullPipelineOrder[] = {"Shadow", "GBuffer", "Lighting",
    //                            "ForwardOpaque", "Transparent",
    //                            "PostProcess", "UI"};
    // R5+ deferred slots still absent: Shadow / GBuffer / Lighting
    // require deferred pipeline plumbing (MRT, shadow map samplers,
    // multiple UBOs — see design.md:457-461). PostProcessPass is
    // wired as a no-op slot today (see PostProcessPass.h P0 doc);
    // R5+ will replace its execute() body with a fullscreen triangle
    // + FBO ping-pong.
    //
    // The pipeline's lifetime is tied to Impl; addPass() is called
    // once in the Impl ctor.
    detail::RenderPipeline        pipeline;

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

    Impl()
        : resources(adapter, shaderPool)
    {
        pipeline.addPass(std::make_unique<detail::ForwardOpaquePass>());
        pipeline.addPass(std::make_unique<detail::TransparentPass>());
        // P0 (2026-07-20) — slot kFullPipelineOrder[5] = "PostProcess".
        // The pass is intentionally a no-op; RenderPipeline dispatches
        // it in order so R5+ can replace the execute() body without
        // re-wiring the pipeline.
        pipeline.addPass(std::make_unique<detail::PostProcessPass>());
        pipeline.addPass(std::make_unique<detail::UIPass>());
    }
};

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

    if (!_impl->adapter.initialize(bgfxParams)) {
        return false;
    }

    _impl->initDesc        = desc;
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

    // View 1: 3D into the editor viewport; must not clear (would wipe chrome).
    _impl->compositeSceneViewId = 1;
    _impl->adapter.setViewClearNone(1);
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

    const uint8_t viewId = _impl->compositeSceneViewId >= 0
                               ? static_cast<uint8_t>(_impl->compositeSceneViewId)
                               : detail::ForwardOpaquePass::kMainViewId;

    _impl->lastDrawCalls = 0;

    // U1+ — dispatched via RenderPipeline::executeAll in registration
    // order [ForwardOpaque, Transparent, UI]. ForwardOpaquePass writes
    // the depth buffer first; TransparentPass then reuses that depth
    // for STATE_DEPTH_TEST_LESS but never writes its own Z so
    // transparent fragments composite over the opaque result without
    // occluding each other (back-to-front sort is deferred to U1++).
    // UIPass ignores the viewId arg and delegates to its injected
    // UIRenderBackend (see UIPass.h:35-47 for the chrome lifecycle
    // contract — flushBatches is intentionally NOT called here in U1+,
    // the host lambda that drives UIManager::render still owns the
    // active flush).
    //
    // Per-pass isEnabled() guards are honored by the pipeline; today
    // all three default to true (set by the RenderPass base ctor).
    _impl->lastDrawCalls = _impl->pipeline.executeAll(
        _impl->adapter, _impl->shaderPool, scene,
        _impl->resources.meshes(), _impl->resources.textures(),
        _impl->resources.materials(),
        _impl->viewportX, _impl->viewportY,
        _impl->viewportW, _impl->viewportH,
        frame,
        viewId);
}

void Renderer::resize(uint32_t width, uint32_t height)
{
    if (!_impl || !_impl->adapter.isInitialized()) {
        return;
    }

    _impl->initDesc.width  = width;
    _impl->initDesc.height = height;
    _impl->adapter.resetResolution(width, height, _impl->initDesc.vsync);
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

    float viewBx[16];
    float projBx[16];
    const bx::Vec3 eyeBx = {eye.x, eye.y, eye.z};
    const bx::Vec3 atBx  = {at.x, at.y, at.z};
    const bx::Vec3 upBx  = {up.x, up.y, up.z};
    bx::mtxLookAt(viewBx, eyeBx, atBx, upBx);
    bx::mtxProj(projBx, fovYDegrees, aspect, nearZ, farZ,
                bgfx::getCaps()->homogeneousDepth);

    ayt::math::Float4x4 view;
    ayt::math::Float4x4 proj;
    std::memcpy(view.ptr(), viewBx, sizeof(viewBx));
    std::memcpy(proj.ptr(), projBx, sizeof(projBx));
    _impl->mainCameraPosition = eye;
    setMainCamera(view, proj);
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
    for (auto& pass : _impl->pipeline.passes()) {
        if (pass && pass->name() == "UI") {
            static_cast<detail::UIPass*>(pass.get())->setBackend(backend);
            return;
        }
    }
}

} // namespace ayt::render
