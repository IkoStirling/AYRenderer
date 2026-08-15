// BgfxSanityDemo.cpp — minimal native bgfx rotating cube (vertex colors, no AYShader/AYResource)

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

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include "AYShadercDriver.h"
#include "AYIO/File.h"
#include "AYPath.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

#ifndef AY_BGFX_SHADER_INCLUDE_HINT
#  define AY_BGFX_SHADER_INCLUDE_HINT ""
#endif

namespace {

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 720;

const char* kVaryingDef = R"(
vec4 v_color0    : COLOR0    = vec4(1.0, 0.0, 0.0, 1.0);

vec3 a_position  : POSITION;
vec4 a_color0    : COLOR0;
)";

const char* kVsColor = R"(
$input a_position, a_color0
$output v_color0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_color0 = a_color0;
}
)";

const char* kFsColor = R"(
$input v_color0

#include <bgfx_shader.sh>

void main()
{
    gl_FragColor = v_color0;
}
)";

struct PosColorVertex {
    float    x;
    float    y;
    float    z;
    uint32_t abgr;
};

struct DemoState {
    bool running = true;
};

const PosColorVertex kCubeVertices[] = {
    {-1.0f,  1.0f,  1.0f, 0xff0000ff},
    { 1.0f,  1.0f,  1.0f, 0xff00ff00},
    {-1.0f, -1.0f,  1.0f, 0xff0000ff},
    { 1.0f, -1.0f,  1.0f, 0xff00ff00},
    {-1.0f,  1.0f, -1.0f, 0xffff0000},
    { 1.0f,  1.0f, -1.0f, 0xffffff00},
    {-1.0f, -1.0f, -1.0f, 0xffff0000},
    { 1.0f, -1.0f, -1.0f, 0xffffff00},
};

const uint16_t kCubeIndices[] = {
    0, 1, 2, 1, 3, 2,
    4, 6, 5, 5, 6, 7,
    0, 2, 4, 4, 2, 6,
    1, 5, 3, 5, 7, 3,
    0, 4, 1, 4, 5, 1,
    2, 3, 6, 3, 7, 6,
};

bool pathExists(const std::string& path)
{
    return !path.empty() && ayt::io::File::exists(path);
}

std::string resolveExistingPath(const char* hint, const char* const* fallbacks, size_t count)
{
    if (hint != nullptr && hint[0] != '\0') {
        const std::string absHint = ayt::io::path::absolute(hint);
        if (pathExists(absHint)) {
            return absHint;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const std::string absPath = ayt::io::path::absolute(fallbacks[i]);
        if (pathExists(absPath)) {
            return absPath;
        }
    }
    return hint != nullptr ? std::string(hint) : std::string{};
}

std::string resolveShadercPath()
{
    static const char* kFallbacks[] = {
        "../../AYShader/thirdParty/bgfx-install/debug/bin/shaderc.exe",
        "../AYShader/thirdParty/bgfx-install/debug/bin/shaderc.exe",
        "../../../AYShader/thirdParty/bgfx-install/debug/bin/shaderc.exe",
    };
    return resolveExistingPath(AY_SHADER_SHADERC_HINT, kFallbacks, sizeof(kFallbacks) / sizeof(kFallbacks[0]));
}

std::string resolveIncludeDir()
{
    static const char* kFallbacks[] = {
        "../../AYShader/thirdParty/bgfx-install/debug/include/bgfx",
        "../AYShader/thirdParty/bgfx-install/debug/include/bgfx",
        "../../../AYShader/thirdParty/bgfx-install/debug/include/bgfx",
    };
    return resolveExistingPath(AY_BGFX_SHADER_INCLUDE_HINT, kFallbacks, sizeof(kFallbacks) / sizeof(kFallbacks[0]));
}

bool compileStage(ayt::shader::AYShadercDriver& driver,
                  const char* name,
                  const char* stage,
                  const std::string& source,
                  const std::string& includeDir,
                  std::vector<uint8_t>& outBytes)
{
    ayt::shader::ShaderCompileRequest req;
    req.scSource           = source;
    req.varyingdefSource   = kVaryingDef;
    req.stage              = stage;
    req.platform           = "windows";
    req.profile            = "s_5_0";
    req.includeDirs        = {includeDir};
    req.outputName         = name;

    const ayt::shader::ShaderCompileResult result = driver.compile(req);
    if (!result.ok) {
        std::fprintf(stderr, "[BgfxSanity] %s compile failed:\n%s\n",
                     name, result.stderrText.c_str());
        return false;
    }

    outBytes = result.bytes;
    return true;
}

bgfx::ShaderHandle createShaderFromBytes(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) {
        return BGFX_INVALID_HANDLE;
    }
    const bgfx::Memory* mem = bgfx::copy(bytes.data(), static_cast<uint32_t>(bytes.size()));
    return bgfx::createShader(mem);
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
    wc.lpszClassName = L"AYRendererBgfxSanityDemo";
    RegisterClassExW(&wc);

    RECT rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"AYRenderer — Native bgfx Sanity (vertex color cube)",
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

    const std::string shadercPath = resolveShadercPath();
    const std::string includeDir  = resolveIncludeDir();
    std::fprintf(stderr, "[BgfxSanity] shaderc: %s (exists=%d)\n",
                 shadercPath.c_str(), pathExists(shadercPath) ? 1 : 0);
    std::fprintf(stderr, "[BgfxSanity] include: %s (exists=%d)\n",
                 includeDir.c_str(), pathExists(includeDir) ? 1 : 0);

    if (!pathExists(shadercPath) || !pathExists(includeDir)) {
        std::fprintf(stderr, "[BgfxSanity] shaderc or bgfx_shader.sh include dir missing.\n");
        return 1;
    }

    std::vector<uint8_t> vsBytes;
    std::vector<uint8_t> fsBytes;
    try {
        ayt::shader::AYShadercDriver driver(shadercPath);
        if (!compileStage(driver, "vs_color", "vertex", kVsColor, includeDir, vsBytes)) {
            return 1;
        }
        if (!compileStage(driver, "fs_color", "fragment", kFsColor, includeDir, fsBytes)) {
            return 1;
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[BgfxSanity] AYShadercDriver init failed: %s\n", ex.what());
        return 1;
    }

    HWND hwnd = createDemoWindow(&state);
    if (!hwnd) {
        std::fprintf(stderr, "[BgfxSanity] failed to create Win32 window.\n");
        return 1;
    }

    bgfx::Init init;
    init.type     = bgfx::RendererType::Count;
    init.vendorId = BGFX_PCI_ID_NONE;
    init.platformData.nwh = hwnd;
    init.resolution.width  = static_cast<uint32_t>(kWindowWidth);
    init.resolution.height = static_cast<uint32_t>(kWindowHeight);
    init.resolution.reset  = BGFX_RESET_VSYNC;

    if (!bgfx::init(init)) {
        std::fprintf(stderr, "[BgfxSanity] bgfx::init failed.\n");
        return 1;
    }

    std::fprintf(stderr, "[BgfxSanity] renderer: %s\n",
                 bgfx::getRendererName(bgfx::getRendererType()));

    bgfx::ShaderHandle vsh = createShaderFromBytes(vsBytes);
    bgfx::ShaderHandle fsh = createShaderFromBytes(fsBytes);
    if (!bgfx::isValid(vsh) || !bgfx::isValid(fsh)) {
        std::fprintf(stderr, "[BgfxSanity] failed to create GPU shaders.\n");
        bgfx::shutdown();
        return 1;
    }

    bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh, true);
    bgfx::destroy(vsh);
    bgfx::destroy(fsh);
    if (!bgfx::isValid(program)) {
        std::fprintf(stderr, "[BgfxSanity] bgfx::createProgram failed.\n");
        bgfx::shutdown();
        return 1;
    }

    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    const bgfx::Memory* vbMem = bgfx::copy(kCubeVertices, sizeof(kCubeVertices));
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vbMem, layout);

    const bgfx::Memory* ibMem = bgfx::copy(kCubeIndices, sizeof(kCubeIndices));
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(ibMem);

    if (!bgfx::isValid(vbh) || !bgfx::isValid(ibh)) {
        std::fprintf(stderr, "[BgfxSanity] failed to create GPU buffers.\n");
        bgfx::destroy(program);
        bgfx::shutdown();
        return 1;
    }

    std::fprintf(stderr, "[BgfxSanity] draw loop started (expect multi-color cube).\n");

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

        if (!state.running || !IsWindow(hwnd)) {
            break;
        }

        const auto now      = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - startTime).count();
        const float aspect  = static_cast<float>(kWindowWidth)
                            / static_cast<float>(kWindowHeight);

        float view[16];
        float proj[16];
        bx::mtxLookAt(view, bx::Vec3{0.0f, 0.0f, -4.0f}, bx::Vec3{0.0f, 0.0f, 0.0f}, bx::Vec3{0.0f, 1.0f, 0.0f});
        bx::mtxProj(proj, 60.0f, aspect, 0.1f, 100.0f, bgfx::getCaps()->homogeneousDepth);

        bgfx::setViewRect(0, 0, 0, static_cast<uint16_t>(kWindowWidth),
                          static_cast<uint16_t>(kWindowHeight));
        bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x303540ff, 1.0f, 0);
        bgfx::setViewTransform(0, view, proj);

        float model[16];
        bx::mtxRotateXY(model, elapsed * 0.6f, elapsed * 0.35f);

        bgfx::setTransform(model);
        bgfx::setVertexBuffer(0, vbh);
        bgfx::setIndexBuffer(ibh);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
                     | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW);
        bgfx::submit(0, program);

        bgfx::frame();

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            state.running = false;
        }
    }

    bgfx::destroy(vbh);
    bgfx::destroy(ibh);
    bgfx::destroy(program);
    bgfx::shutdown();

    std::printf("AYRenderer_BgfxSanityDemo finished.\n");
    return 0;
}
