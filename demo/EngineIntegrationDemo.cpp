// EngineIntegrationDemo.cpp — GameLoop + Entity ECS + RendererSubSystem (Win32)

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

#include "AYEntity.h"
#include "AYEntityModule.h"
#include "AYGameLoop.h"
#include "AYRendererSubSystem.h"
#include "aymath/MathUtils.h"

#include "assetsImpl/AYMaterial.h"
#include "assetsImpl/AYMesh.h"
#include "assetsImpl/AYTexture.h"

#include "ayio/File.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 720;

const char* kSimpleLitPhoskia = R"(
material SimpleLit {
    texture2d albedoMap
    uniform vec3 lightDir
    uniform vec3 lightColor
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)

    vertex {
        in pos : position
        in nrm : normal
        in uv  : texcoord
        out worldNormal : normal = (modelMatrix * vec4(nrm, 0.0)).xyz
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in worldNormal : normal
        in uvOut : texcoord
        let albedo = sample(albedoMap, uvOut) * baseColor
        let ndotl = max(dot(normalize(worldNormal), normalize(lightDir)), 0.05)
        return vec4(albedo.rgb * lightColor * ndotl, albedo.a)
    }
}
)";

struct DemoAssets {
    std::string assetDir;
    std::string meshPath;
    std::string materialPath;
};

struct DemoState {
    ayt::game::GameLoop* loop = nullptr;
    bool                   running = true;
};

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

bool writeBytes(const std::string& path, const void* data, size_t size)
{
    ayt::io::File file(path, ayt::io::File::Mode::BinaryWrite);
    if (!file.isOpen()) {
        return false;
    }
    return file.write(data, size) == size;
}

bool writeText(const std::string& path, const std::string& text)
{
    ayt::io::File file(path, ayt::io::File::Mode::Write);
    if (!file.isOpen()) {
        return false;
    }
    return file.write(text.data(), text.size()) == text.size();
}

bool ensureAssetDirectory(const std::string& path)
{
#ifdef _WIN32
    if (CreateDirectoryA(path.c_str(), nullptr) != 0) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    (void)path;
    return true;
#endif
}

bool bakeDemoAssets(const std::string& rootPrefix, DemoAssets& out)
{
    if (!ensureAssetDirectory(rootPrefix)) {
        return false;
    }

    out.assetDir     = rootPrefix + "assets\\";
    out.meshPath     = out.assetDir + "cube.aymesh";
    out.materialPath = out.assetDir + "cube.aymat";

    if (!ensureAssetDirectory(out.assetDir)) {
        return false;
    }

    const std::string shaderPath  = out.assetDir + "simple_lit.phoskia";
    const std::string texturePath = out.assetDir + "albedo.aytex";

    if (!writeText(shaderPath, kSimpleLitPhoskia)) {
        return false;
    }

    ayt::resource::Texture texture;
    texture.createCheckerboard(64, 64, 8);
    std::vector<ayt::resource::UInt8> texBinary;
    if (!texture.saveToBinary(texBinary)) {
        return false;
    }
    if (!writeBytes(texturePath, texBinary.data(), texBinary.size())) {
        return false;
    }

    ayt::resource::Material material;
    material.setShader("simple_lit.phoskia");
    const ayt::resource::Float32 baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    material.setFloat4("baseColor", baseColor);
    material.setTexture("albedoMap", "albedo.aytex");
    std::vector<ayt::resource::UInt8> matBinary;
    if (!material.saveToBinary(matBinary)) {
        return false;
    }
    if (!writeBytes(out.materialPath, matBinary.data(), matBinary.size())) {
        return false;
    }

    ayt::resource::Mesh mesh;
    mesh.createCube(1.0f);
    std::vector<ayt::resource::UInt8> meshBinary;
    if (!mesh.saveToBinary(meshBinary)) {
        return false;
    }
    if (!writeBytes(out.meshPath, meshBinary.data(), meshBinary.size())) {
        return false;
    }

    std::fprintf(stderr, "[EngineDemo] baked assets in %s\n", out.assetDir.c_str());
    return true;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DemoState* state = reinterpret_cast<DemoState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CLOSE:
        if (state != nullptr) {
            state->running = false;
            if (state->loop != nullptr) {
                state->loop->stop();
            }
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND createDemoWindow(DemoState* state)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = windowProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AYEngineIntegrationDemo";
    RegisterClassExW(&wc);

    RECT rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"AY Engine Integration — ECS + GameLoop + Renderer",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);

    if (hwnd != nullptr) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

void pumpWin32Messages(DemoState& state)
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            state.running = false;
            if (state.loop != nullptr) {
                state.loop->stop();
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace

int main()
{
    DemoState state;
    ayt::game::GameLoop& loop = ayt::game::GameLoop::instance();
    state.loop = &loop;

    HWND hwnd = createDemoWindow(&state);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "[EngineDemo] failed to create Win32 window\n");
        return 1;
    }

    ayt::render::RendererSubSystem::setBootstrapWindow(
        hwnd,
        static_cast<uint32_t>(kWindowWidth),
        static_cast<uint32_t>(kWindowHeight));

    char tempDir[MAX_PATH] = {};
    std::string assetRootPrefix = "engine_demo_assets\\";
    if (GetTempPathA(MAX_PATH, tempDir) > 0) {
        assetRootPrefix = std::string(tempDir) + "ayengine_integration\\";
    }

    DemoAssets assets;
    if (!bakeDemoAssets(assetRootPrefix, assets)) {
        std::fprintf(stderr, "[EngineDemo] bakeDemoAssets failed\n");
        return 1;
    }

    const std::string shaderDumpDir = assetRootPrefix + "shader_dump\\";
    if (ensureAssetDirectory(shaderDumpDir)) {
        ayt::render::RendererSubSystem::setBootstrapShaderDumpDirectory(shaderDumpDir);
        std::fprintf(stderr, "[EngineDemo] shader dump dir: %s\n", shaderDumpDir.c_str());
    }

    std::fprintf(stderr, "[EngineDemo] mesh='%s'\n", assets.meshPath.c_str());
    std::fprintf(stderr, "[EngineDemo] material='%s'\n", assets.materialPath.c_str());
    std::fprintf(stderr, "[EngineDemo] shaderc hint: %s (exists=%d)\n",
                 AY_SHADER_SHADERC_HINT, fileExists(AY_SHADER_SHADERC_HINT) ? 1 : 0);
    std::fprintf(stderr, "[EngineDemo] Esc to quit | debug overlay on\n");

    const auto startTime = std::chrono::steady_clock::now();
    ayt::entity::Entity* cubeEntity = nullptr;

    const uint64_t listenerId = loop.onUpdate([&](float /*deltaTime*/) {
        pumpWin32Messages(state);

        if (!state.running) {
            loop.stop();
            return;
        }

        if (cubeEntity == nullptr) {
            cubeEntity = ayt::entity::World::instance().createEntity();
            if (cubeEntity != nullptr) {
                cubeEntity->addComponent<ayt::entity::Transform>();
                auto* mesh = cubeEntity->addComponent<ayt::entity::MeshComponent>();
                mesh->meshPath     = assets.meshPath;
                mesh->materialPath = assets.materialPath;
                std::fprintf(stderr, "[EngineDemo] spawned ECS cube entity\n");
            }
        }

        if (cubeEntity != nullptr) {
            auto* transform = cubeEntity->getComponent<ayt::entity::Transform>();
            if (transform != nullptr) {
                const auto now      = std::chrono::steady_clock::now();
                const float elapsed = std::chrono::duration<float>(now - startTime).count();
                const ayt::math::FQuaternion rotation =
                    ayt::math::FQuaternion::fromEulerAngles(
                        ayt::math::FVector3(elapsed * 0.5f, elapsed * 0.8f, 0.0f));
                transform->rotation = rotation;
            }
        }

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            state.running = false;
            loop.stop();
        }
    });

    loop.setTargetFPS(60.0f);
    loop.setRenderThreadEnabled(false);

    ayt::entity::bootstrapModule();
    ayt::render::RendererSubSystem::registerSubSystem();

    loop.run();
    loop.offUpdate(listenerId);
    loop.shutdown();

    std::printf("AYEngineIntegration_Demo finished.\n");
    return 0;
}
