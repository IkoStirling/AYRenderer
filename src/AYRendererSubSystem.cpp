#include "AYRendererSubSystem.h"

#include <AYSubSystemRegistry.h>

#include <cstdio>
#include <string>

namespace ayt::render
{

namespace {

void*    g_bootstrapWindow = nullptr;
uint32_t g_bootstrapWidth  = 1280;
uint32_t g_bootstrapHeight = 720;
std::string g_bootstrapShaderDumpDir;

} // namespace

void RendererSubSystem::setBootstrapWindow(void* nativeWindowHandle, uint32_t width,
                                           uint32_t height)
{
    g_bootstrapWindow = nativeWindowHandle;
    g_bootstrapWidth  = width;
    g_bootstrapHeight = height;
}

void RendererSubSystem::setBootstrapShaderDumpDirectory(const std::string& dir)
{
    g_bootstrapShaderDumpDir = dir;
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

    if (_windowHandle == nullptr) {
        std::fprintf(stderr, "[RendererSubSystem] bootstrap window not set\n");
        return false;
    }

    InitDesc desc;
    desc.windowHandle       = _windowHandle;
    desc.width              = _width;
    desc.height             = _height;
    desc.vsync              = true;
    desc.backend            = Backend::Auto;
    desc.enableDebugOverlay  = true;

    if (!_renderer.initialize(desc)) {
        std::fprintf(stderr, "[RendererSubSystem] renderer initialize failed\n");
        return false;
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
    std::fprintf(stderr, "[RendererSubSystem] initialized (%ux%u)\n", _width, _height);
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

void RendererSubSystem::renderFrame()
{
    if (!_ready) {
        return;
    }

    auto& loop = ayt::game::GameLoop::instance();
    const float aspect = static_cast<float>(_width) / static_cast<float>(_height);
    _renderer.setMainCameraLookAtPerspective(
        ayt::math::FVector3(0.0f, 0.0f, 4.0f),
        ayt::math::FVector3(0.0f, 0.0f, 0.0f),
        ayt::math::FVector3(0.0f, 1.0f, 0.0f),
        60.0f, aspect, 0.1f, 100.0f);

    _scene.clear();
    if (_sceneBuilder) {
        _sceneBuilder(_scene);
    }

    ClearDesc clear;
    clear.r = 0.08f;
    clear.g = 0.09f;
    clear.b = 0.12f;
    clear.a = 1.0f;

    _renderer.beginFrame(clear);
    _renderer.render(_scene);
    _renderer.endFrame();
    _renderer.pollShaderHotReload();
}

REGISTER_SUBSYSTEM(RendererSubSystem, {"Entity"}, 100);

} // namespace ayt::render
