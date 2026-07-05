#include "AYRendererSubSystem.h"



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



} // namespace



void RendererSubSystem::setBootstrapWindow(void* nativeWindowHandle, uint32_t width,

                                           uint32_t height)

{

    g_bootstrapWindow = nativeWindowHandle;

    g_bootstrapWidth  = width;

    g_bootstrapHeight = height;

}



void RendererSubSystem::setBootstrapViewport(uint16_t x, uint16_t y, uint16_t width,

                                             uint16_t height)

{

    g_bootstrapViewportX = x;

    g_bootstrapViewportY = y;

    g_bootstrapViewportW = width;

    g_bootstrapViewportH = height;

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

        .dependencies = {"Entity"},

        .basePriority = 100,

        .timeType     = ayt::game::SubSystemDescriptor::TimeType::Scaled,

    };

    return desc;

}



bool RendererSubSystem::initialize()

{

    _windowHandle = g_bootstrapWindow;

    _width        = g_bootstrapWidth;

    _height       = g_bootstrapHeight;

    _viewportX    = g_bootstrapViewportX;

    _viewportY    = g_bootstrapViewportY;

    _viewportW    = g_bootstrapViewportW;

    _viewportH    = g_bootstrapViewportH;



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

    desc.backend            = Backend::Auto;

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

                                  ayt::math::FVector3(1.0f, 0.96f, 0.88f));



    auto& loop = ayt::game::GameLoop::instance();

    loop.setRenderThreadEnabled(false);

    loop.setRenderCallback([this]() { renderFrame(); });



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

    _sceneBuilder = std::move(callback);

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

    _renderer.setMainCameraLookAtPerspective(
        ayt::math::FVector3(0.0f, 0.0f, 4.0f),
        ayt::math::FVector3(0.0f, 0.0f, 0.0f),
        ayt::math::FVector3(0.0f, 1.0f, 0.0f),
        60.0f, aspect, 0.1f, 100.0f);

    _scene.clear();
    if (_sceneBuilder) {
        _sceneBuilder(_scene);
    }

    _renderer.render(_scene);
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

    _renderer.setViewportRect(0, 0, static_cast<uint16_t>(_width), static_cast<uint16_t>(_height));
    _renderer.beginFrame(clear);

    if (renderScene3D && _viewportW >= 32 && _viewportH >= 32) {
        _renderer.setViewportRect(_viewportX, _viewportY, _viewportW, _viewportH);
        renderScenePass();
    }

    if (uiBackend != nullptr) {
        uiBackend->setFramebufferSize(static_cast<uint16_t>(_width), static_cast<uint16_t>(_height));
        uiPass(renderScene3D);
    }

    _renderer.setDebugOverlaySuppressed(true);
    _renderer.endFrame();
    _renderer.setDebugOverlaySuppressed(false);
    _renderer.pollShaderHotReload();
}



REGISTER_SUBSYSTEM(RendererSubSystem, {"Entity"}, 100);



} // namespace ayt::render

