#pragma once

#include "AYRenderer.h"

#include <AYGameLoop.h>

#include <functional>

namespace ayt::render
{

class UIRenderBackend;

using SceneBuildCallback = std::function<void(RenderScene&)>;
using CompositeUiPass    = std::function<void(bool skipViewportPanel)>;

// GameLoop subsystem: owns Renderer, submits frames via render callback.
class RendererSubSystem : public ayt::game::ISubSystem {
public:
    static void setBootstrapWindow(void* nativeWindowHandle, uint32_t width, uint32_t height);
    static void setBootstrapViewport(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
    static void setBootstrapShaderDumpDirectory(const std::string& dir);
    static void setBootstrapShaderCacheDirectory(const std::string& dir);

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

private:
    void renderFrame();
    void renderScenePass();

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
};

} // namespace ayt::render
