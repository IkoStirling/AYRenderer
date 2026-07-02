#pragma once

#include "AYRenderer.h"

#include <AYGameLoop.h>

#include <functional>

namespace ayt::render
{

using SceneBuildCallback = std::function<void(RenderScene&)>;

// GameLoop subsystem: owns Renderer, submits frames via render callback.
class RendererSubSystem : public ayt::game::ISubSystem {
public:
    static void setBootstrapWindow(void* nativeWindowHandle, uint32_t width, uint32_t height);
    static void setBootstrapShaderDumpDirectory(const std::string& dir);

    const char* getName() const override { return "Renderer"; }
    const ayt::game::SubSystemDescriptor& getDescriptor() const override;

    bool initialize() override;
    void update(float deltaTime) override;
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    void setSceneBuilder(SceneBuildCallback callback);
    Renderer& renderer();
    RenderScene& renderScene();

    static RendererSubSystem* findRegistered();

private:
    void renderFrame();

    Renderer      _renderer;
    RenderScene   _scene;
    SceneBuildCallback _sceneBuilder;
    void*         _windowHandle = nullptr;
    uint32_t      _width        = 1280;
    uint32_t      _height       = 720;
    bool          _ready        = false;
};

} // namespace ayt::render
