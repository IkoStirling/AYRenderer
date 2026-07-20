#pragma once

#include "AYRenderer.h"

#include <AYGameLoop.h>

// INT-04 (2026-07-20): Renderer -> EventBus bridge. EventBusHostScope is the
// host-side RAII container from AYApplication that owns the WindowResize
// subscription. RendererSubSystem is a pure consumer of WindowResizeEvent
// (DeviceSubSystem produces it via INT-03); the handler calls
// Renderer::resize(width, height) which wraps bgfx::reset.
#include <AYAppEventHost.h>
#include <ayevent/Events/WindowEvents.h>

#include <cstddef>
#include <functional>

namespace ayt::render
{

class UIRenderBackend;

using SceneBuildCallback = std::function<void(RenderScene&)>;
// AI-1 (2026-07-20): CompositeUiPass now takes an enum so the host
// can run the populate half before Renderer::render (which dispatches
// UIPass::execute that flushes text) and the flush half after. See
// RendererSubSystem::renderCompositeFrame for the dispatch order.
//
// Pre-AI-1 the callback took only `bool skipViewportPanel` and ran
// populate+flush in one go inside UIManager::render. The split is
// required so the RenderPass dispatch can own the UI submission
// boundary (UIPass::execute calls backend->flushBatches after the
// widget walk has populated the batch).
enum class CompositeUiPhase {
    Populate = 0,  // host runs UIManager::populateFrame — walk widget
                   // tree, accumulate batches on backend
    Flush    = 1,  // host runs UIManager::flushFrame — close
                   // IRenderBackend lifecycle (endCanvas + endFrame,
                   // which internally calls flushColoredRects)
};
using CompositeUiPass = std::function<void(bool skipViewportPanel, CompositeUiPhase phase)>;

// Supplies the native window handle + size at initialize() time. Lets the
// application wire the renderer to a window source (e.g. AYDevice's
// DeviceSubSystem) without the renderer depending on that module's headers.
// Returns false if no window is available yet.
using WindowProvider = std::function<bool(void*& outHandle, uint32_t& outWidth, uint32_t& outHeight)>;

// GameLoop subsystem: owns Renderer, submits frames via render callback.
class RendererSubSystem : public ayt::game::ISubSystem {
public:
    static void setBootstrapWindow(void* nativeWindowHandle, uint32_t width, uint32_t height);
    static void setBootstrapViewport(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
    static void setBootstrapBackend(Backend backend);
    static void setBootstrapShaderDumpDirectory(const std::string& dir);
    static void setBootstrapShaderCacheDirectory(const std::string& dir);

    // Optional window source, preferred over the static bootstrap window when
    // set. Call before the subsystem initializes (i.e. before GameLoop::run()).
    static void setWindowProvider(WindowProvider provider);

    const char* getName() const override { return "Renderer"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override;

    bool initialize() override;
    void update(float deltaTime) override;
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    void setClientSize(uint32_t width, uint32_t height);
    void setViewportRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height);

    void setSceneBuilder(SceneBuildCallback callback);
    Renderer& renderer();
    RenderScene& renderScene();

    static RendererSubSystem* findRegistered();

    // Explicit registration (replaces static REGISTER_SUBSYSTEM for static-lib safety).
    static void registerSubSystem();

    bool isReady() const { return _ready; }
    void renderCompositeFrame(bool renderScene3D, UIRenderBackend* uiBackend, CompositeUiPass uiPass);

    // F1 diag — sizeof as seen by the AYRenderer static lib TU.
    // Test binary compares against its own sizeof(); mismatch ⇒ ODR /
    // incremental-build layout bug (not an EventBus logic bug).
    static std::size_t diagSizeofRenderScene();
    static std::size_t diagSizeofRendererSubSystem();
    static std::size_t diagSizeofFrameContext();
    static int diagFlagLight();
    static int diagFlagFrameShadow();
    static int diagFlagDefaultShadow();

private:
    void renderFrame();
    void renderScenePass();

    // INT-04: WindowResize handler. Triggered by EventBus pump (main thread,
    // sync — Phase 4 contract) when DeviceSubSystem posts a delta. Calls
    // Renderer::resize(width, height) which wraps bgfx::reset.
    void onWindowResize(const ayt::event::WindowResizeEvent& e);

    Renderer           _renderer;
    RenderScene        _scene;
    SceneBuildCallback _sceneBuilder;
    void*              _windowHandle = nullptr;
    uint32_t           _width        = 1280;
    uint32_t           _height       = 720;
    uint16_t           _viewportX    = 0;
    uint16_t           _viewportY    = 0;
    uint16_t           _viewportW    = 1280;
    uint16_t           _viewportH    = 720;
    bool               _ready        = false;

    // INT-04: host-side EventBus host scope (Phase 4 lesson applied). The
    // renderer is a pure consumer today — the scope owns the WindowResize
    // subscription registered in initialize() and released in shutdown().
    // Dtor is a no-op so a forgotten disconnect() cannot re-open the
    // shutdown-time SIGSEGV path documented in [[ay-event-system]] §Phase 4.
    ayt::app::EventBusHostScope _events;
};

} // namespace ayt::render
