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
    std::string shaderPath;
    std::string texturePath;
};

struct DemoState {
    ayt::render::Renderer* renderer = nullptr;
    bool running = true;
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
        std::fprintf(stderr, "[Demo] writeBytes failed to open: %s\n", path.c_str());
        return false;
    }
    if (file.write(data, size) != size) {
        std::fprintf(stderr, "[Demo] writeBytes failed to write: %s\n", path.c_str());
        return false;
    }
    return true;
}

bool writeText(const std::string& path, const std::string& text)
{
    ayt::io::File file(path, ayt::io::File::Mode::Write);
    if (!file.isOpen()) {
        std::fprintf(stderr, "[Demo] writeText failed to open: %s\n", path.c_str());
        return false;
    }
    if (file.write(text.data(), text.size()) != text.size()) {
        std::fprintf(stderr, "[Demo] writeText failed to write: %s\n", path.c_str());
        return false;
    }
    return true;
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
        std::fprintf(stderr, "[Demo] failed to create root dir: %s\n", rootPrefix.c_str());
        return false;
    }

    out.assetDir     = rootPrefix + "assets\\";
    out.shaderPath   = out.assetDir + "simple_lit.phoskia";
    out.texturePath  = out.assetDir + "albedo.aytex";
    out.materialPath = out.assetDir + "cube.aymat";
    out.meshPath     = out.assetDir + "cube.aymesh";

    if (!ensureAssetDirectory(out.assetDir)) {
        std::fprintf(stderr, "[Demo] failed to create asset dir: %s\n", out.assetDir.c_str());
        return false;
    }

    if (!writeText(out.shaderPath, kSimpleLitPhoskia)) {
        return false;
    }

    ayt::resource::Texture texture;
    texture.createCheckerboard(64, 64, 8);
    std::vector<ayt::resource::UInt8> texBinary;
    if (!texture.saveToBinary(texBinary)) {
        std::fprintf(stderr, "[Demo] texture.saveToBinary failed\n");
        return false;
    }
    if (!writeBytes(out.texturePath, texBinary.data(), texBinary.size())) {
        return false;
    }

    ayt::resource::Material material;
    material.setShader("simple_lit.phoskia");
    const ayt::resource::Float32 baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    material.setFloat4("baseColor", baseColor);
    material.setTexture("albedoMap", "albedo.aytex");
    std::vector<ayt::resource::UInt8> matBinary;
    if (!material.saveToBinary(matBinary)) {
        std::fprintf(stderr, "[Demo] material.saveToBinary failed\n");
        return false;
    }
    if (!writeBytes(out.materialPath, matBinary.data(), matBinary.size())) {
        return false;
    }

    ayt::resource::Mesh mesh;
    mesh.createCube(1.0f);
    std::vector<ayt::resource::UInt8> meshBinary;
    if (!mesh.saveToBinary(meshBinary)) {
        std::fprintf(stderr, "[Demo] mesh.saveToBinary failed\n");
        return false;
    }
    if (!writeBytes(out.meshPath, meshBinary.data(), meshBinary.size())) {
        return false;
    }

    ayt::resource::Mesh verifyMesh;
    if (!verifyMesh.load(out.meshPath)) {
        std::fprintf(stderr, "[Demo] verify mesh.load failed: %s\n", out.meshPath.c_str());
        return false;
    }

    ayt::resource::Material verifyMat;
    if (!verifyMat.load(out.materialPath)) {
        std::fprintf(stderr, "[Demo] verify material.load failed: %s\n", out.materialPath.c_str());
        return false;
    }

    std::fprintf(stderr, "[Demo] baked assets in %s\n", out.assetDir.c_str());
    std::fprintf(stderr, "  mesh     %s (%zu bytes, v=%u)\n",
                 out.meshPath.c_str(), meshBinary.size(), verifyMesh.getVertexCount());
    std::fprintf(stderr, "  material %s (shader=%s)\n",
                 out.materialPath.c_str(), verifyMat.getShader());
    std::fprintf(stderr, "  texture  %s\n", out.texturePath.c_str());
    std::fprintf(stderr, "  shader   %s\n", out.shaderPath.c_str());
    return true;
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
        0, wc.lpszClassName, L"AYRenderer — Rotating Cube (R4 debug overlay)",
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
    init.enableDebugOverlay = true;

    if (!renderer.initialize(init)) {
        std::fprintf(stderr, "Renderer initialize failed (bgfx init).\n");
        return 1;
    }

    std::fprintf(stderr, "[Demo] shaderc hint: %s (exists=%d)\n",
                 AY_SHADER_SHADERC_HINT, fileExists(AY_SHADER_SHADERC_HINT) ? 1 : 0);

    char tempDir[MAX_PATH] = {};
    std::string assetRootPrefix = "demo_assets\\";
    if (GetTempPathA(MAX_PATH, tempDir) > 0) {
        assetRootPrefix = std::string(tempDir) + "ayrenderer_demo\\";
    }

    DemoAssets assets;
    if (!bakeDemoAssets(assetRootPrefix, assets)) {
        std::fprintf(stderr, "[Demo] bakeDemoAssets failed.\n");
        renderer.shutdown();
        return 1;
    }

    const std::string shaderDumpDir = assetRootPrefix + "shader_dump\\";
    if (ensureAssetDirectory(shaderDumpDir)) {
        renderer.setShaderIntermediateDumpDirectory(shaderDumpDir);
        std::fprintf(stderr, "[Demo] shader .sc dump dir: %s\n", shaderDumpDir.c_str());
    }

    ayt::render::MeshHandle mesh = renderer.loadMesh(assets.meshPath);
    if (!mesh.isValid()) {
        std::fprintf(stderr, "[Demo] renderer.loadMesh failed: %s\n", assets.meshPath.c_str());
        renderer.shutdown();
        return 1;
    }

    ayt::render::MaterialHandle material = renderer.loadMaterial(assets.materialPath);
    if (!material.isValid()) {
        std::fprintf(stderr, "[Demo] renderer.loadMaterial failed: %s\n",
                     assets.materialPath.c_str());
        renderer.destroyMesh(mesh);
        renderer.shutdown();
        return 1;
    }

    std::fprintf(stderr, "[Demo] bridge load OK (mesh id=%llu material id=%llu)\n",
                 static_cast<unsigned long long>(mesh.id),
                 static_cast<unsigned long long>(material.id));
    std::fprintf(stderr, "[Demo] shader hot-reload enabled (edit %s while running)\n",
                 assets.shaderPath.c_str());
    std::fprintf(stderr, "[Demo] debug overlay enabled (FPS / draw stats, top-left)\n");
    std::fprintf(stderr, "[Demo] press F9 to save screenshot.tga / screenshot.png under asset root\n");
    std::fprintf(stderr, "[Demo] (F9 avoids Visual Studio F12 = Go To Definition while debugging)\n");

    const std::string screenshotBase = assetRootPrefix + "screenshot";

    renderer.setDirectionalLight(ayt::math::FVector3(0.35f, -0.85f, -0.4f),
                                 ayt::math::FVector3(1.0f, 0.96f, 0.88f));

    ayt::render::RenderScene scene;
    scene.add(mesh, material);

    const auto startTime = std::chrono::steady_clock::now();
    bool f9WasDown = false;

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

        const bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9Down && !f9WasDown) {
            if (renderer.captureScreenshot(screenshotBase)) {
                std::fprintf(stderr, "[Demo] screenshot queued: %s\n", screenshotBase.c_str());
            } else {
                std::fprintf(stderr, "[Demo] screenshot request failed\n");
            }
        }
        f9WasDown = f9Down;

        renderer.endFrame();

        // Poll after the frame is submitted so bgfx never draws with a half-swapped shader.
        renderer.pollShaderHotReload();

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
