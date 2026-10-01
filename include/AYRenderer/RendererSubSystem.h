#pragma once

#include "AYRenderer.h"
#include "AYRenderer/SceneVisibility.h"

#include <AYGameLoop.h>

// Renderer owns its WindowResize listener through the event module's explicit
// subscription lifetime container. RendererSubSystem is a pure consumer of WindowResizeEvent
// (DeviceSubSystem produces it via INT-03); the handler calls
// Renderer::resize(width, height) which wraps bgfx::reset.
#include <AYEventSystem/Events/WindowEvents.h>
#include <AYEventSystem/SubscriptionScope.h>

#include <atomic>
#include <cstddef>
#include <functional>

namespace ayt::render
{

class UIRenderBackend;

// §P1 L5 (2026-08-24) — named default camera so the editor freecam
// fallback is no longer a bare magic (4, 3, 5). Used when no
// camera override is set (legacy / non-GameLoop callers) and as the
// staging ground before an Editor freecam takes over.
inline constexpr float kDefaultEditorCameraEyeX = 4.0f;
inline constexpr float kDefaultEditorCameraEyeY = 3.0f;
inline constexpr float kDefaultEditorCameraEyeZ = 5.0f;

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
    [[nodiscard]] uint16_t viewportWidth() const noexcept { return _viewportW; }
    [[nodiscard]] uint16_t viewportHeight() const noexcept { return _viewportH; }
    [[nodiscard]] float viewportAspect() const noexcept {
        return _viewportH > 0u
            ? static_cast<float>(_viewportW) / static_cast<float>(_viewportH)
            : 1.0f;
    }

    // Legacy/global builders remain active regardless of the current ECS
    // World (standalone demos use this path). World-owned builders must use
    // addSceneBuilderForOwner so a World can detach every callback before its
    // systems are destroyed.
    void setSceneBuilder(SceneBuildCallback callback);
    void addSceneBuilderForOwner(const void* owner, SceneBuildCallback callback);
    void clearSceneBuildersForOwner(const void* owner);
    Renderer& renderer();
    RenderScene& renderScene();

    // Editor freecam / host camera. When enabled, renderScenePass uses
    // this look-at instead of the built-in default (4,3,5)→origin.
    void setCameraLookAt(const ayt::math::FVector3& eye,
                         const ayt::math::FVector3& at,
                         const ayt::math::FVector3& up,
                         float fovYDegrees = 50.0f);
    void setCameraMatrices(const ayt::math::Float4x4& view,
                           const ayt::math::Float4x4& projection,
                           const ayt::math::FVector3& position);
    void clearCameraOverride();
    bool hasCameraOverride() const noexcept { return _cameraOverride; }

    // Edit-mode preview override for Camera Overlay 2D. Runtime scene builders
    // still author their camera first; this host override is applied last and
    // is cleared when the editor enters Play.
    void setOverlayCamera2DOverride(
        const ayt::math::Float4x4& view,
        const ayt::math::Float4x4& projection,
        uint32_t layerMask = 0xFFFFFFFFu);
    void clearOverlayCamera2DOverride();
    bool hasOverlayCamera2DOverride() const noexcept;
    /// Read the viewport override for frame extraction/culling. False leaves output unchanged.
    bool overlayCamera2DOverride(OverlayCamera2D& output) const noexcept;

    // Editor/diagnostic presentation filter. The source RenderScene remains
    // complete and the override is applied only to the packet passed to
    // Renderer::render. Runtime hosts keep the default all-visible behavior.
    void setSceneVisibilityFilter(const SceneVisibilityFilter& filter);
    void clearSceneVisibilityFilter();
    bool hasSceneVisibilityFilter() const noexcept;

    static RendererSubSystem* findRegistered();

    // Explicit registration (replaces static REGISTER_SUBSYSTEM for static-lib safety).
    static void registerSubSystem();

    bool isReady() const { return _ready; }
    void renderCompositeFrame(bool renderScene3D, UIRenderBackend* uiBackend, CompositeUiPass uiPass);

    // F1 diag — sizeof as seen by the AYRenderer static lib TU.
    // Test binary compares against its own sizeof(); mismatch ⇒ ODR /
    // incremental-build layout bug (not an EventBus logic bug).
    //
    // §5.5 cleanup (2026-07-22): diagFlagLight() / diagFlagFrameShadow()
    // removed — the diagnostic compile flags are permanently 0 now
    // (see include/AYRenderer/F1DiagFlags.h). Callers should compare against the
    // AY_F1_DIAG_* macros directly. diagFlagDefaultShadow() was retired
    // in E4.
    static std::size_t diagSizeofRenderScene();
    static std::size_t diagSizeofRendererSubSystem();
    static std::size_t diagSizeofFrameContext();

private:
    void renderFrame();
    void renderScenePass();
    void applySceneCameraOverrides(RenderScene& scene);
    // Keep the Renderer camera current before scene builders query it for
    // CPU visibility. This is also called immediately when Editor freecam or
    // viewport aspect changes.
    void syncMainCamera();

    // INT-04: WindowResize handler. Triggered by EventBus pump (main thread,
    // sync — Phase 4 contract) when DeviceSubSystem posts a delta. Calls
    // Renderer::resize(width, height) which wraps bgfx::reset.
    void onWindowResize(const ayt::event::WindowResizeEvent& e);

    Renderer           _renderer;
    RenderScene        _scene;
    SceneBuildCallback _sceneBuilder;
    // §P1 L2 (2026-08-24) — atomic valid flag so the scene packet
    // can't be torn between the renderFrame() polling path and the
    // renderScenePass() consumer path if a future caller drives them
    // from different threads (editor preview pane on worker thread,
    // say). Today both are main-thread, but the load cost is zero and
    // it future-proofs the contract.
    uint64_t           _scenePacketFrame = 0;
    float              _scenePacketInterpolationAlpha = 0.0f;
    std::atomic<bool>  _scenePacketValid{false};
    void*              _windowHandle = nullptr;
    uint32_t           _width        = 1280;
    uint32_t           _height       = 720;
    uint16_t           _viewportX    = 0;
    uint16_t           _viewportY    = 0;
    uint16_t           _viewportW    = 1280;
    uint16_t           _viewportH    = 720;
    bool               _ready        = false;

    bool               _cameraOverride = false;
    ayt::math::FVector3 _camEye{4.0f, 3.0f, 5.0f};
    ayt::math::FVector3 _camAt{0.0f, 0.0f, 0.0f};
    ayt::math::FVector3 _camUp{0.0f, 1.0f, 0.0f};
    float               _camFovYDegrees = 50.0f;

    // INT-04: EventBus subscription scope (Phase 4 lesson applied). The
    // renderer is a pure consumer today — the scope owns the WindowResize
    // subscription registered in initialize() and released in shutdown().
    // Dtor is a no-op so a forgotten disconnect() cannot re-open the
    // shutdown-time SIGSEGV path documented in [[ay-event-system]] §Phase 4.
    ayt::event::SubscriptionScope _events;
};

} // namespace ayt::render
