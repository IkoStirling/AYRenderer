// RotatingCubeDemo.cpp — windowed smoke test: AYRenderer + AYShader (Win32)

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

#include <chrono>
#include <cstdio>
#include <string>

namespace {

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 720;

const char* kRotatingCubePhoskia = R"(
material RotatingCube {
    property baseColor = vec4(0.25, 0.55, 0.95, 1.0)

    vertex {
        in pos : position
        return u_modelViewProj * vec4(pos, 1.0)
    }
    fragment {
        return baseColor
    }
}
)";

struct DemoState {
    ayt::render::Renderer* renderer = nullptr;
    bool running = true;
};

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
        0, wc.lpszClassName, L"AYRenderer — Rotating Cube",
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

    ayt::render::MaterialHandle material =
        renderer.createMaterialFromPhoskia(kRotatingCubePhoskia, "demo_rotating_cube");
    if (!material.isValid()) {
        std::fprintf(stderr, "Material acquire failed.\n");
        renderer.shutdown();
        return 1;
    }

    ayt::render::MeshHandle mesh = renderer.createUnitCube();
    if (!mesh.isValid()) {
        std::fprintf(stderr, "Failed to create unit cube mesh.\n");
        renderer.destroyMaterial(material);
        renderer.shutdown();
        return 1;
    }

    ayt::render::RenderScene scene;
    scene.add(mesh, material);

    renderer.setMaterialColor(material, "baseColor", 0.25f, 0.55f, 0.95f, 1.0f);

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

        const auto now     = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - startTime).count();
        const float aspect   = static_cast<float>(kWindowWidth)
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
