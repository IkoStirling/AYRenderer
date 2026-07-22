#include "AYRendererSubSystem.h"

#include "AYF1DiagFlags.h"
#include "AYUIRenderBackend.h"

#include <AYSubSystemRegistry.h>

#include <cstdio>
#include <string>



namespace ayt::render

{



namespace {



void*    g_bootstrapWindow = nullptr;

uint32_t g_bootstrapWidth  = 1280;

uint32_t g_bootstrapHeight = 720;

uint16_t g_bootstrapViewportX = 0;

uint16_t g_bootstrapViewportY = 0;

uint16_t g_bootstrapViewportW = 1280;

uint16_t g_bootstrapViewportH = 720;

std::string g_bootstrapShaderDumpDir;

std::string g_bootstrapShaderCacheDir;

Backend g_bootstrapBackend = Backend::Auto;

WindowProvider g_windowProvider;



} // namespace



void RendererSubSystem::setBootstrapWindow(void* nativeWindowHandle, uint32_t width,

                                           uint32_t height)

{

    g_bootstrapWindow = nativeWindowHandle;

    g_bootstrapWidth  = width;

    g_bootstrapHeight = height;

}



void RendererSubSystem::setWindowProvider(WindowProvider provider)

{

    g_windowProvider = std::move(provider);

}



void RendererSubSystem::setBootstrapViewport(uint16_t x, uint16_t y, uint16_t width,

                                             uint16_t height)

{

    g_bootstrapViewportX = x;

    g_bootstrapViewportY = y;

    g_bootstrapViewportW = width;

    g_bootstrapViewportH = height;

}



void RendererSubSystem::setBootstrapBackend(Backend backend)

{

    g_bootstrapBackend = backend;

}



void RendererSubSystem::setBootstrapShaderDumpDirectory(const std::string& dir)

{

    g_bootstrapShaderDumpDir = dir;

}



void RendererSubSystem::setBootstrapShaderCacheDirectory(const std::string& dir)

{

    g_bootstrapShaderCacheDir = dir;

}



const ayt::game::SubSystemDescriptor& RendererSubSystem::getDescriptor() const

{

    static ayt::game::SubSystemDescriptor desc = {

        .name         = "Renderer",

        .dependencies = {"Entity", "Device"},

        .basePriority = 100,

        .timeType     = ayt::game::SubSystemDescriptor::TimeType::Scaled,

    };

    return desc;

}



bool RendererSubSystem::initialize()

{

    // Prefer a window provider (e.g. AYDevice's DeviceSubSystem) when set;
    // otherwise fall back to the static bootstrap window (editor / demos).

    void*    providedHandle = nullptr;

    uint32_t providedWidth  = 0;

    uint32_t providedHeight = 0;

    if (g_windowProvider && g_windowProvider(providedHandle, providedWidth, providedHeight)

        && providedHandle != nullptr) {

        _windowHandle = providedHandle;

        _width        = providedWidth;

        _height       = providedHeight;

        // Default the viewport to the full provided window (client builds have
        // no separate viewport panel like the editor does).

        _viewportX    = 0;

        _viewportY    = 0;

        _viewportW    = static_cast<uint16_t>(providedWidth);

        _viewportH    = static_cast<uint16_t>(providedHeight);

    } else {

        _windowHandle = g_bootstrapWindow;

        _width        = g_bootstrapWidth;

        _height       = g_bootstrapHeight;

        _viewportX    = g_bootstrapViewportX;

        _viewportY    = g_bootstrapViewportY;

        _viewportW    = g_bootstrapViewportW;

        _viewportH    = g_bootstrapViewportH;

    }



    if (_windowHandle == nullptr) {

        std::fprintf(stderr, "[RendererSubSystem] bootstrap window not set\n");

        return false;

    }



    if (_viewportW < 32 || _viewportH < 32) {

        std::fprintf(stderr, "[RendererSubSystem] viewport too small (%ux%u)\n",

                     static_cast<unsigned>(_viewportW),

                     static_cast<unsigned>(_viewportH));

        return false;

    }



    InitDesc desc;

    desc.windowHandle       = _windowHandle;

    desc.width              = _width;

    desc.height             = _height;

    desc.vsync              = true;

    desc.backend            = g_bootstrapBackend;

    desc.enableDebugOverlay  = false;



    if (!_renderer.initialize(desc)) {

        std::fprintf(stderr, "[RendererSubSystem] renderer initialize failed\n");

        return false;

    }



    _renderer.setViewportRect(_viewportX, _viewportY, _viewportW, _viewportH);



    if (!g_bootstrapShaderCacheDir.empty()) {

        _renderer.setShaderCacheDirectory(g_bootstrapShaderCacheDir);

        std::fprintf(stderr, "[RendererSubSystem] shader cache dir: %s\n",

                     g_bootstrapShaderCacheDir.c_str());

    }



    if (!g_bootstrapShaderDumpDir.empty()) {

        _renderer.setShaderIntermediateDumpDirectory(g_bootstrapShaderDumpDir);

        std::fprintf(stderr, "[RendererSubSystem] shader dump dir: %s\n",

                     g_bootstrapShaderDumpDir.c_str());

    }



    _renderer.setDirectionalLight(ayt::math::FVector3(0.35f, -0.85f, -0.4f),

                                  ayt::math::FVector3(1.35f, 1.28f, 1.15f));



    auto& loop = ayt::game::GameLoop::instance();

    loop.setRenderThreadEnabled(false);

    loop.setRenderCallback([this]() { renderFrame(); });

    // INT-04 (2026-07-20): subscribe WindowResizeEvent on the EventBus. The
    // handler runs on the main thread (Phase 4 contract: emit/pump are
    // main-thread-only). DeviceSubSystem (INT-03) posts resize deltas; the
    // pump() call in GameLoop::tickOnceFrame() flushes them into this
    // listener before our update() runs.
    _events.subscribe<ayt::event::WindowResizeEvent>(
        [this](const ayt::event::WindowResizeEvent& e) {
            this->onWindowResize(e);
        });



    _ready = true;

    std::fprintf(stderr, "[RendererSubSystem] initialized client %ux%u viewport (%u,%u %ux%u)\n",

                 _width, _height,

                 static_cast<unsigned>(_viewportX), static_cast<unsigned>(_viewportY),

                 static_cast<unsigned>(_viewportW), static_cast<unsigned>(_viewportH));

    return true;

}



void RendererSubSystem::update(float /*deltaTime*/)

{

}



void RendererSubSystem::fixedUpdate(float /*fixedDeltaTime*/)

{

}



void RendererSubSystem::shutdown()

{

    if (_ready) {

        auto& loop = ayt::game::GameLoop::instance();

        loop.setRenderCallback({});

        _sceneBuilder = nullptr;

        _renderer.shutdown();

        _ready = false;

    }

    // INT-04: release the WindowResize subscription BEFORE the renderer
    // teardown finishes. Phase 4 lesson: explicit disconnect(), never dtor
    // touches the bus.
    _events.disconnect();

    std::fprintf(stderr, "[RendererSubSystem] shutdown\n");

}



void RendererSubSystem::setClientSize(uint32_t width, uint32_t height)

{

    _width  = width;

    _height = height;

    if (_ready) {

        _renderer.resize(width, height);

    }

}



void RendererSubSystem::setViewportRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height)

{

    _viewportX = x;

    _viewportY = y;

    _viewportW = width;

    _viewportH = height;

    if (_ready) {

        _renderer.setViewportRect(x, y, width, height);

    }

}



void RendererSubSystem::setSceneBuilder(SceneBuildCallback callback)
{
    // Phase 1 SC-01: scene builders form an ordered chain. The first
    // registered callback runs first; subsequent ones are appended.
    // This lets multiple ECS systems (RenderSystem + SkinnedMeshRenderSystem)
    // share the same per-frame scene buffer without one overwriting
    // the other. Order of registration = order of execution.
    if (!callback) return;
    if (_sceneBuilder) {
        SceneBuildCallback previous = _sceneBuilder;
        _sceneBuilder = [previous, callback](RenderScene& scene) {
            previous(scene);
            callback(scene);
        };
    } else {
        _sceneBuilder = std::move(callback);
    }
}



Renderer& RendererSubSystem::renderer()

{

    return _renderer;

}



RenderScene& RendererSubSystem::renderScene()

{

    return _scene;

}



RendererSubSystem* RendererSubSystem::findRegistered()

{

    auto* system = ayt::game::SubSystemRegistry::instance().findSubSystem("Renderer");

    return dynamic_cast<RendererSubSystem*>(system);

}



void RendererSubSystem::renderScenePass()
{
    const float aspect = static_cast<float>(_viewportW) / static_cast<float>(_viewportH);

    // Elevated 3/4 view so the ground plane reads as a square and the
    // cube sits on top (eye at y=0 made the ground a thin diamond and
    // put the near ground edge in front of the cube).
    _renderer.setMainCameraLookAtPerspective(
        ayt::math::FVector3(4.0f, 3.0f, 5.0f),
        ayt::math::FVector3(0.0f, 0.0f, 0.0f),
        ayt::math::FVector3(0.0f, 1.0f, 0.0f),
        50.0f, aspect, 0.1f, 100.0f);

    _scene.clear();
    if (_sceneBuilder) {
        _sceneBuilder(_scene);
    }

    _renderer.render(_scene);
}

// INT-04: WindowResize handler. Called on the main thread by EventBus pump
// (Phase 4 contract). Forwards to Renderer::resize which wraps bgfx::reset.
void RendererSubSystem::onWindowResize(const ayt::event::WindowResizeEvent& e)
{
    if (!_renderer.isInitialized()) {
        // Device may post a WindowResizeEvent before the renderer initializes
        // (init order: Device -> Renderer in GameLoop descriptor). Drop the
        // event silently — Renderer's stored _width/_height will be picked up
        // from the window provider at initialize() time, so the first frame
        // is already at the right size.
        return;
    }
    _renderer.resize(e.width, e.height);
    std::fprintf(stderr, "[RendererSubSystem] WindowResize -> %ux%u\n",
                 static_cast<unsigned>(e.width), static_cast<unsigned>(e.height));
}

void RendererSubSystem::renderFrame()

{

    if (!_ready || _viewportW < 32 || _viewportH < 32) {

        return;

    }

    ClearDesc clear;

    clear.r = 0.08f;

    clear.g = 0.09f;

    clear.b = 0.12f;

    clear.a = 1.0f;



    _renderer.beginFrame(clear);

    renderScenePass();

    _renderer.endFrame();

    _renderer.pollShaderHotReload();

}



void RendererSubSystem::renderCompositeFrame(bool renderScene3D, UIRenderBackend* uiBackend,
                                             CompositeUiPass uiPass)
{
    if (!_ready || _width < 32 || _height < 32 || !uiPass) {
        return;
    }

    ClearDesc clear;
    clear.r = 0.10f;
    clear.g = 0.10f;
    clear.b = 0.11f;
    clear.a = 1.0f;

    // Full-window clear on view 0; 3D uses view 1; UI uses view 2 (CLEAR_NONE).
    // Shrinking view 0 to the 3D hole left chrome (splitter) pixels uncleared.
    _renderer.beginCompositeFrame(clear, static_cast<uint16_t>(_width),
                                  static_cast<uint16_t>(_height));

    // AI-1 (2026-07-20): dispatch order — populate UI BEFORE 3D
    // renderScenePass so the widget walk accumulates batches that
    // UIPass::execute (dispatched inside renderScenePass) will flush
    // via backend->flushBatches(). Pre-AI-1 the order was reversed
    // (3D first, then UI lambda) and the UI flush lived entirely in
    // the host lambda, bypassing the RenderPass dispatch.
    if (uiBackend != nullptr) {
        uiBackend->setFramebufferSize(static_cast<uint16_t>(_width), static_cast<uint16_t>(_height));
        uiPass(renderScene3D, CompositeUiPhase::Populate);
    }

    if (renderScene3D && _viewportW >= 32 && _viewportH >= 32) {
        _renderer.setViewportRect(_viewportX, _viewportY, _viewportW, _viewportH);
        renderScenePass();  // dispatches [ForwardOpaque, Transparent, UIPass];
                            // UIPass::execute now flushes pending text
    }

    // Flush half — close the IRenderBackend lifecycle (endCanvas +
    // endFrame; endFrame flushes pendingRects via flushColoredRects).
    // This call is REQUIRED in AI-1: without it the backend stays in
    // an open-frame state and the next beginFrame is undefined.
    uiPass(renderScene3D, CompositeUiPhase::Flush);

    _renderer.setDebugOverlaySuppressed(true);
    _renderer.endFrame();
    _renderer.setDebugOverlaySuppressed(false);
    _renderer.pollShaderHotReload();
}



void RendererSubSystem::registerSubSystem()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    if (findRegistered() != nullptr) {
        return;
    }
    ::ayt::game::IGameLoop::instance().registerSubSystem(new RendererSubSystem());
}

std::size_t RendererSubSystem::diagSizeofRenderScene()
{
    return sizeof(RenderScene);
}

std::size_t RendererSubSystem::diagSizeofRendererSubSystem()
{
    return sizeof(RendererSubSystem);
}

std::size_t RendererSubSystem::diagSizeofFrameContext()
{
    // Implemented in AYRenderer.cpp so this TU never includes FrameContext
    // (avoids bgfx / MemorySystem header collisions when diag flags are on).
    return ayt::render::detailDiagSizeofFrameContext();
}

// §5.5 cleanup (2026-07-22) — diagFlagLight() / diagFlagFrameShadow()
// removed. The diagnostic compile flags they returned are now permanently
// 0 (see include/AYF1DiagFlags.h). Keeping the functions around as
// "always-0" stubs would add surface for no gain — callers (notably
// Test_F1_LayoutDiag) now assert directly against the AY_F1_DIAG_*
// header macros at compile time. diagFlagDefaultShadow() was already
// retired in E4.

} // namespace ayt::render

