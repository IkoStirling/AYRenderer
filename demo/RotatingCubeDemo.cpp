// RotatingCubeDemo.cpp — windowed smoke test: AYRenderer + AYShader + AYResource (Win32)

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

#include "AYRenderer.h"
#include "AYMathUtils.h"

#include "assetsImpl/AYMaterial.h"
#include "assetsImpl/AYMesh.h"
#include "assetsImpl/AYTexture.h"

#include "AYFile.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 720;

const char* kSimpleLitPhoskia = R"(
material SimpleLit {
    texture2d albedoMap
    uniform vec3 cameraPos
    uniform vec3 lightDir
    uniform vec3 lightColor
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)

    vertex {
        in pos : position
        in nrm : normal
        in uv  : texcoord
        out worldNormal : normal = vec3(0.0, 0.0, 1.0)
        out uvOut : texcoord = vec2(0.0, 0.0)
        return u_modelViewProj * vec4(pos, 1.0)
    }
    fragment {
        in worldNormal : normal
        in uvOut : texcoord
        let albedo = sample(albedoMap, uvOut) * baseColor
        let N = normalize(worldNormal)
        let L = normalize(-lightDir)
        let ndotl = max(dot(N, L), 0.05)
        return vec4(albedo.rgb * lightColor * ndotl, albedo.a)
    }
}
)";

struct DemoAssets {
    std::string meshPath;
    std::string materialPath;
    std::string shaderPath;
    std::string texturePath;
};

struct DemoState {
    ayt::render::Renderer* renderer = nullptr;
    bool running = true;
};

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

DemoAssets bakeDemoAssets(const std::string& prefix)
{
    DemoAssets paths;
    paths.shaderPath  = prefix + "demo_simple_lit.phoskia";
    paths.texturePath = prefix + "demo_albedo.aytex";
    paths.materialPath = prefix + "demo_cube.aymat";
    paths.meshPath    = prefix + "demo_cube.aymesh";

    writeText(paths.shaderPath, kSimpleLitPhoskia);

    ayt::resource::Texture texture;
    texture.createCheckerboard(64, 64, 8);
    std::vector<ayt::resource::UInt8> texBinary;
    texture.saveToBinary(texBinary);
    writeBytes(paths.texturePath, texBinary.data(), texBinary.size());

    ayt::resource::Material material;
    material.setShader("demo_simple_lit.phoskia");
    material.setTexture("albedoMap", "demo_albedo.aytex");
    std::vector<ayt::resource::UInt8> matBinary;
    material.saveToBinary(matBinary);
    writeBytes(paths.materialPath, matBinary.data(), matBinary.size());

    ayt::resource::Mesh mesh;
    mesh.createCube(1.0f);
    std::vector<ayt::resource::UInt8> meshBinary;
    mesh.saveToBinary(meshBinary);
    writeBytes(paths.meshPath, meshBinary.data(), meshBinary.size());

    return paths;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DemoState* state = reinterpret_cast<DemoState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CLOSE:
        if (state) {
            state->running = false;
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
    wc.lpszClassName = L"AYRendererRotatingCubeDemo";
    RegisterClassExW(&wc);

    RECT rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"AYRenderer — Rotating Cube (R2b/R3)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);

    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

} // namespace

int main()
{
    DemoState state;
    ayt::render::Renderer renderer;
    state.renderer = &renderer;

    HWND hwnd = createDemoWindow(&state);
    if (!hwnd) {
        std::fprintf(stderr, "Failed to create Win32 window.\n");
        return 1;
    }

    ayt::render::InitDesc init;
    init.windowHandle = hwnd;
    init.width        = static_cast<uint32_t>(kWindowWidth);
    init.height       = static_cast<uint32_t>(kWindowHeight);
    init.vsync        = true;
    init.backend      = ayt::render::Backend::Auto;

    if (!renderer.initialize(init)) {
        std::fprintf(stderr, "Renderer initialize failed (bgfx init).\n");
        return 1;
    }

    char tempDir[MAX_PATH] = {};
    std::string assetPrefix = "demo_assets_";
    if (GetTempPathA(MAX_PATH, tempDir) > 0) {
        assetPrefix = std::string(tempDir) + "ayrenderer_";
    }

    const DemoAssets assets = bakeDemoAssets(assetPrefix);

    ayt::render::MeshHandle mesh = renderer.loadMesh(assets.meshPath);
    ayt::render::MaterialHandle material = renderer.loadMaterial(assets.materialPath);
    if (!mesh.isValid() || !material.isValid()) {
        std::fprintf(stderr, "Failed to load demo assets via AYResource bridge.\n");
        renderer.shutdown();
        return 1;
    }

    renderer.setDirectionalLight(ayt::math::FVector3(0.35f, -0.85f, -0.4f),
                                 ayt::math::FVector3(1.0f, 0.96f, 0.88f));

    ayt::render::RenderScene scene;
    scene.add(mesh, material);

    const auto startTime = std::chrono::steady_clock::now();

    MSG msg{};
    while (state.running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                state.running = false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!state.running) {
            break;
        }

        if (!IsWindow(hwnd)) {
            break;
        }

        const auto now      = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - startTime).count();
        const float aspect    = static_cast<float>(kWindowWidth)
                              / static_cast<float>(kWindowHeight);

        using namespace ayt::math;

        const Float4x4 model = rotate(FVector3(0.0f, 1.0f, 0.0f), elapsed * 0.8f)
                             * rotate(FVector3(1.0f, 0.0f, 0.0f), elapsed * 0.5f);

        scene.clear();
        scene.add(mesh, material, model);
        renderer.setMainCameraLookAtPerspective(FVector3(0.0f, 0.0f, 4.0f),
                                              FVector3(0.0f, 0.0f, 0.0f),
                                              FVector3(0.0f, 1.0f, 0.0f),
                                              60.0f, aspect, 0.1f, 100.0f);

        ayt::render::ClearDesc clear;
        clear.r = 0.08f;
        clear.g = 0.09f;
        clear.b = 0.12f;
        clear.a = 1.0f;

        renderer.beginFrame(clear);
        renderer.render(scene);
        renderer.endFrame();

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            state.running = false;
        }
    }

    renderer.destroyMesh(mesh);
    renderer.destroyMaterial(material);
    renderer.shutdown();

    std::printf("AYRenderer_Demo finished.\n");
    return 0;
}
