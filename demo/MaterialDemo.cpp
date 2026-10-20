// Loads the authored .mat library through the same resource bridge as runtime content.
#define UNICODE
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "AYRenderer.h"
#include "AYRenderer/UIRenderBackend.h"
#include "AYResource/ResourceManager.h"
#include "AYMath/MathUtils.h"
#include "ProceduralGalleryGeometry.h"
#include "MaterialExhibitContent.h"
#include "MaterialLibraryValidation.h"
#include "DemoUtf8Arguments.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <set>

namespace
{
namespace fs = std::filesystem;
const auto &ids = ayt::render::demo::kMaterialIds;
const auto &scenes = ayt::render::demo::kMaterialSceneNames;
const char *variants[] = {"base", "clean", "wet", "worn"};
const char *lightRigs[] = {"studio", "sunset", "inspection"};
std::string utf8(const fs::path &p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
struct State
{
    bool running = true;
};
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM key, LPARAM extra)
{
    auto *state = reinterpret_cast<State *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_CLOSE || (message == WM_KEYDOWN && key == VK_ESCAPE))
    {
        if (state)
            state->running = false;
        return 0;
    }
    return DefWindowProcW(window, message, key, extra);
}
} // namespace
int runMaterials(int argc, char **argv)
{
    using namespace ayt::render;
    using namespace ayt::math;
    bool batch = false, motion = false, hidden = false, animate = false, sweepLights = false,
         freezeSurface = false;
    int selectedScene = -1, hero = -1, variant = 0, lightRig = 0;
    Backend backend = Backend::Direct3D11;
    fs::path output = "material-captures", root = AY_MATERIAL_LIBRARY_HINT;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--gallery")
            batch = true;
        else if (arg == "--motion-gallery")
        {
            batch = true;
            motion = true;
            animate = true;
        }
        else if (arg == "--animate")
            animate = true;
        else if (arg == "--lighting-sweep")
            sweepLights = true;
        else if (arg == "--freeze-surface")
            freezeSurface = true;
        else if (arg == "--hidden")
            hidden = true;
        else if (arg == "--d3d12")
            backend = Backend::Direct3D12;
        else if (arg == "--output" && i + 1 < argc)
            output = fs::u8path(argv[++i]);
        else if (arg == "--materials" && i + 1 < argc)
            root = fs::u8path(argv[++i]);
        else if (arg == "--variant" && i + 1 < argc)
        {
            const std::string v = argv[++i];
            variant = -1;
            for (int j = 0; j < 4; ++j)
                if (v == variants[j])
                    variant = j;
            if (variant < 0)
            {
                std::fprintf(stderr, "Unknown variant\n");
                return 2;
            }
        }
        else if (arg == "--lighting" && i + 1 < argc)
        {
            const std::string v = argv[++i];
            lightRig = -1;
            for (int j = 0; j < 3; ++j)
                if (v == lightRigs[j])
                    lightRig = j;
            if (lightRig < 0)
            {
                std::fprintf(stderr, "Unknown lighting rig\n");
                return 2;
            }
        }
        else if (arg == "--scene" && i + 1 < argc)
        {
            const std::string v = argv[++i];
            for (int j = 0; j < int(scenes.size()); ++j)
                if (v == scenes[j])
                    selectedScene = j;
            if (selectedScene < 0)
            {
                std::fprintf(stderr, "Unknown scene\n");
                return 2;
            }
        }
        else if (arg == "--material" && i + 1 < argc)
        {
            const std::string v = argv[++i];
            for (int j = 0; j < int(ids.size()); ++j)
                if (v == ids[j])
                    hero = j;
            if (hero < 0)
            {
                std::fprintf(stderr, "Unknown material\n");
                return 2;
            }
        }
        else
        {
            std::fprintf(stderr,
                         "AYRenderer_MaterialDemo [--gallery|--motion-gallery] [--animate] "
                         "[--lighting-sweep] [--freeze-surface] [--hidden] [--d3d12] [--materials "
                         "directory] [--output "
                         "directory] [--variant base|clean|wet|worn] [--lighting "
                         "studio|sunset|inspection] [--scene "
                         "gallery|facade|workbench|study|cutout|shapes|collection|shapes-extra|transparency] "
                         "[--material library-id]\n");
            return 2;
        }
    }
    if (freezeSurface)
        animate = false;
    if (selectedScene >= 3 && hero >= 0)
    {
        std::fprintf(stderr,
                     "Study/cutout/shapes scenes use their authored material sets; omit --material\n");
        return 2;
    }
    fs::create_directories(output);
    root = fs::absolute(root);
    // Check the complete selected condition before creating a device or a window.
    std::vector<const char *> selectedIds(ids.begin(), ids.end());
    for (const auto &study : demo::kMaterialStudies)
        selectedIds.push_back(study.id);
    selectedIds.insert(selectedIds.end(), demo::kMaterialCutoutIds.begin(), demo::kMaterialCutoutIds.end());
    for (const auto *id : selectedIds)
    {
        ayt::resource::Material source;
        std::string error;
        if (!demo::readWorkshopMaterial(
                root / (std::string(id) + (variant ? std::string("-") + variants[variant] : "") + ".mat"),
                source, error))
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    }
    constexpr uint16_t width = 1280, height = 800;
    State state;
    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AYMaterialGallery";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowW(wc.lpszClassName, L"PBR Material Library", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                                nullptr, wc.hInstance, nullptr);
    if (!window)
        return 1;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    if (!hidden)
        ShowWindow(window, SW_SHOW);
    Renderer renderer;
    InitDesc init;
    init.windowHandle = window;
    init.width = width;
    init.height = height;
    init.msaa = 0;
    init.vsync = !batch;
    init.backend = backend;
    if (!renderer.initialize(init))
    {
        DestroyWindow(window);
        return 1;
    }
    renderer.setFxaaEnabled(false);
    renderer.setSmaaEnabled(false);
    renderer.setShadowsEnabled(false);
    renderer.setPostProcessGamma(2.2f);
    renderer.setPostProcessExposure(1);
    renderer.setDirectionalLight({-.45f, -.8f, .55f}, {2.2f, 2.05f, 1.9f});
    renderer.setAmbientStrength(.35f);
    UIRenderBackend ui;
    if (!ui.initialize(renderer))
    {
        renderer.shutdown();
        DestroyWindow(window);
        return 1;
    }
    ui.setFramebufferSize(width, height);
    SceneLights lights;
    renderer.setSceneLights(&lights);
    std::array<MaterialHandle, demo::kMaterialIds.size()> materials;
    std::array<MaterialHandle, demo::kMaterialStudies.size()> studyMaterials;
    std::array<MaterialHandle, demo::kMaterialCutoutIds.size()> cutoutMaterials;
    std::vector<std::pair<MaterialHandle, PbrSurfaceSettings>> animatedMaterials;
    std::vector<MaterialHandle> ownedMaterials;
    const auto loadVariant = [&]() {
        bool valid = true;
        std::set<std::string> validatedShaders;
        animatedMaterials.clear();
        const auto load = [&](const fs::path &path) {
            ayt::resource::Material source;
            std::string error;
            if (!demo::readWorkshopMaterial(path, source, error))
            {
                std::fprintf(stderr, "%s\n", error.c_str());
                valid = false;
                return MaterialHandle{};
            }
            const auto file = utf8(path);
            const auto shaderPath = ayt::resource::resolveAssetPath(file, source.getShader());
            MaterialHandle shaderCheck;
            if (validatedShaders.insert(shaderPath).second)
            {
                // The general bridge may substitute a fallback shader. This
                // content viewer must reject a broken authored shader instead
                // of reporting a successful but visually different showcase.
                shaderCheck = renderer.createMaterialFromFile(shaderPath);
                if (!shaderCheck.isValid())
                {
                    std::fprintf(stderr, "Authored shader compilation failed: %s\n", shaderPath.c_str());
                    valid = false;
                    return MaterialHandle{};
                }
            }
            const auto handle = renderer.loadMaterial(file);
            // Retain the validated program until a real material owns it.
            // Retiring the check earlier would invalidate the only token and
            // make the bridge compile the exact same program a second time.
            renderer.destroyMaterial(shaderCheck);
            valid =
                valid && handle.isValid() && !ayt::resource::ResourceManager::instance().hasLoadFailed(file);
            source.forEachParameter([&](const char *name, ayt::resource::MaterialParamType type) {
                if (type == ayt::resource::MaterialParamType::Texture2D)
                {
                    const auto texture = ayt::resource::resolveAssetPath(file, source.getTexture(name));
                    if (ayt::resource::ResourceManager::instance().hasLoadFailed(texture))
                    {
                        std::fprintf(stderr, "Texture decoding failed: %s\n", texture.c_str());
                        valid = false;
                    }
                }
            });
            if (handle.isValid() && std::none_of(ownedMaterials.begin(), ownedMaterials.end(),
                                                 [&](auto h) { return h.id == handle.id; }))
                ownedMaterials.push_back(handle);
            return handle;
        };
        for (int i = 0; i < int(ids.size()); ++i)
        {
            const auto file = utf8(root / (std::string(ids[i]) +
                                           (variant ? std::string("-") + variants[variant] : "") + ".mat"));
            materials[i] = load(fs::u8path(file));
            if (!valid)
                return false;
            animatedMaterials.emplace_back(materials[i], renderer.materialPbrSurfaceSettings(materials[i]));
        }
        for (size_t i = 0; i < studyMaterials.size(); ++i)
        {
            const auto file = utf8(root / (std::string(demo::kMaterialStudies[i].id) +
                                           (variant ? std::string("-") + variants[variant] : "") + ".mat"));
            studyMaterials[i] = load(fs::u8path(file));
            if (!valid)
                return false;
        }
        for (size_t i = 0; i < cutoutMaterials.size(); ++i)
        {
            const auto file = root / (std::string(demo::kMaterialCutoutIds[i]) +
                                      (variant ? std::string("-") + variants[variant] : "") + ".mat");
            cutoutMaterials[i] = load(file);
            if (!valid)
                return false;
            animatedMaterials.emplace_back(cutoutMaterials[i],
                                           renderer.materialPbrSurfaceSettings(cutoutMaterials[i]));
        }
        return valid;
    };
    bool valid = loadVariant();
    const auto cube = demo::createGalleryCube(renderer),
               sphere = demo::uploadGalleryGeometry(renderer, demo::gallerySphere()),
               cylinder = demo::uploadGalleryGeometry(renderer, demo::galleryCylinder()),
               torus = demo::uploadGalleryGeometry(renderer, demo::galleryTorus());
    valid = valid && cube.isValid() && sphere.isValid() && cylinder.isValid() && torus.isValid();
    if (!valid)
    {
        std::fprintf(stderr, "Library or geometry loading failed\n");
        renderer.shutdown();
        DestroyWindow(window);
        return 1;
    }
    bool deferred = false, paused = false, wasAnimating = false;
    int sceneIndex = selectedScene < 0 ? 0 : selectedScene, previousBatch = -1, failures = 0;
    const int count = hero >= 0 || selectedScene >= 0 ? 1 : int(scenes.size()), frames = motion ? 100 : 28;
    uint32_t tick = 0;
    float time = 0;
    auto previous = std::chrono::steady_clock::now();
    std::array<bool, 256> down{};
    const auto pressed = [&](int k) {
        const bool v = (GetAsyncKeyState(k) & 0x8000) != 0;
        const bool edge = v && !down[k];
        down[k] = v;
        return edge;
    };
    std::vector<fs::path> expected;
    std::puts("Tab scene | M material | V variant | L lights | A animate surface | S lighting sweep | F path "
              "| Space pause | F9 capture | Esc quit");
    while (state.running && (!batch || tick < uint32_t(2 * count * frames)))
    {
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - previous).count();
        previous = now;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                state.running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!state.running)
            break;
        const int batchIndex = int(tick / frames), local = int(tick % frames);
        if (batch && batchIndex != previousBatch)
        {
            previousBatch = batchIndex;
            deferred = batchIndex >= count;
            sceneIndex = selectedScene < 0 ? batchIndex % count : selectedScene;
            renderer.configurePipeline(deferred ? RenderPipelineDesc::makeDeferred()
                                                : RenderPipelineDesc::makeDefault());
            renderer.setShadowsEnabled(false);
            renderer.notifyCameraCut();
        }
        if (!batch && GetForegroundWindow() == window)
        {
            if (pressed(VK_TAB))
            {
                hero = -1;
                sceneIndex = (sceneIndex + 1) % int(scenes.size());
                renderer.notifyCameraCut();
            }
            if (pressed('M'))
            {
                if (sceneIndex >= 3)
                    sceneIndex = 0;
                hero = (hero + 1) % int(ids.size());
                renderer.notifyCameraCut();
            }
            if (pressed('V'))
            {
                if (wasAnimating)
                    for (const auto &[handle, base] : animatedMaterials)
                        renderer.setMaterialPbrSurfaceSettings(handle, base);
                variant = (variant + 1) % 4;
                if (!loadVariant())
                {
                    ++failures;
                    state.running = false;
                }
            }
            if (pressed('A'))
                animate = !animate;
            if (pressed('S'))
                sweepLights = !sweepLights;
            if (pressed('L'))
                lightRig = (lightRig + 1) % 3;
            if (pressed(VK_SPACE))
                paused = !paused;
            if (pressed('F'))
            {
                deferred = !deferred;
                renderer.configurePipeline(deferred ? RenderPipelineDesc::makeDeferred()
                                                    : RenderPipelineDesc::makeDefault());
                renderer.setShadowsEnabled(false);
            }
        }
        if (batch)
            time = motion ? float(local) / 30 : .8f;
        else if (!paused)
            time += elapsed;
        lights = {};
        const float lx = sweepLights ? std::sin(time * 1.1f) * .8f : -.45f,
                    lz = sweepLights ? std::cos(time * 1.1f) * .8f : .55f;
        if (lightRig == 0)
        {
            lights.add(Light::directional({lx, -.8f, lz}, {2.2f, 2.05f, 1.9f}));
            lights.add(Light::directional({.8f, -.25f, .2f}, {.55f, .7f, 1.1f}));
            lights.add(Light::directional({-.3f, -.25f, -.9f}, {1.2f, .8f, .45f}));
        }
        else if (lightRig == 1)
        {
            lights.add(Light::directional({lx, -.25f, lz}, {3.f, 1.3f, .55f}));
            lights.add(Light::directional({.8f, -.5f, -.6f}, {.3f, .55f, 1.1f}));
            lights.add(Light::point({0, 3, -3}, 12, 7, {.4f, .7f, 1.f}));
        }
        else
        {
            lights.add(Light::directional({lx, -.9f, lz}, {1.6f, 1.6f, 1.6f}));
            lights.add(Light::directional({.8f, -.2f, .3f}, {1.2f, 1.2f, 1.2f}));
            lights.add(Light::point({-2, 4, -3}, 16, 9, {1, 1, 1}));
        }
        ayt::resource::ResourceManager::instance().update(batch ? 1.f / 30 : elapsed);
        if (animate || wasAnimating)
            for (const auto &[handle, base] : animatedMaterials)
            {
                auto value = base;
                if (animate)
                {
                    value.uvOffsetX += time * .08f;
                    value.normalStrength = 1 + std::sin(time * 1.4f) * .8f;
                    value.roughnessBias = std::sin(time) * .22f;
                }
                renderer.setMaterialPbrSurfaceSettings(handle, value);
            }
        wasAnimating = animate;
        RenderScene scene;
        const auto exhibit = demo::makeMaterialExhibit(sceneIndex, hero);
        renderer.setMainCameraLookAtPerspective(exhibit.cameraPosition, exhibit.cameraTarget, {0, 1, 0},
                                                exhibit.fov, float(width) / height, .1f, 100);
        const std::array<MeshHandle, 4> meshes = {cube, sphere, cylinder, torus};
        for (const auto &object : exhibit.objects)
            scene.add(meshes[size_t(object.mesh)],
                      object.cutout >= 0  ? cutoutMaterials[size_t(object.cutout)]
                      : object.study >= 0 ? studyMaterials[size_t(object.study)]
                                          : materials[object.material],
                      translate(object.position.x, object.position.y, object.position.z) *
                          rotate(FVector3(0, 1, 0), object.angle) *
                          scale(object.scale.x, object.scale.y, object.scale.z));
        ClearDesc clear;
        clear.r = .025f;
        clear.g = .035f;
        clear.b = .05f;
        renderer.beginCompositeFrame(clear, width, height);
        renderer.render(scene);
        ui.beginFrame();
        ui.beginCanvas({0, 0, float(width), float(height)});
        ui.drawRoundedRect({20, 18, float(width) - 20, 95}, {.015f, .025f, .045f, .88f}, 8);
        const std::string caption =
            std::string("MATERIAL WORKSHOP  /  ") + (hero >= 0 ? ids[hero] : scenes[sceneIndex]) + "  /  " +
            variants[variant] + "  /  " + lightRigs[lightRig] + "  /  " + (deferred ? "Deferred" : "Forward");
        ui.drawText({34, 27, float(width) - 34, 53}, std::wstring(caption.begin(), caption.end()), 20,
                    FVector4{.88f, .94f, 1, 1});
        ui.drawText({34, 60, float(width) - 34, 85},
                    L"Tab scene   M material   V condition   L lights   A surface motion   S light sweep   F "
                    L"path   Space pause   F9 capture",
                    13, FVector4{.62f, .73f, .85f, 1});
        if ((sceneIndex == 5 || sceneIndex == 7) && hero < 0)
        {
            ui.drawRoundedRect({20, float(height) - 61, float(width) - 20, float(height) - 18},
                               {.015f, .025f, .045f, .88f}, 8);
            const auto legend =
                sceneIndex == 5
                    ? L"Rows near to far: sphere / scaled sphere / cylinder / torus.  Columns: metal / stone "
                      L"/ wood / ceramic / rubber / paint / fabric / panel."
                    : L"Rows near to far: sphere / scaled sphere / cylinder / torus.  Columns: copper / "
                      L"concrete / terrazzo / leather / asphalt / brick / marble / plastic.";
            ui.drawText({34, float(height) - 51, float(width) - 34, float(height) - 25}, legend, 14,
                        FVector4{.78f, .87f, .97f, 1});
        }
        if ((sceneIndex == 0 || sceneIndex == 6) && hero < 0)
        {
            const auto view = lh::lookAt(exhibit.cameraPosition, exhibit.cameraTarget, {0, 1, 0});
            const auto projection = lh::perspective(radians(exhibit.fov), float(width) / height, .1f, 100);
            const int labels = sceneIndex == 6 ? demo::kOpaqueMaterialCount : demo::kMaterialExhibitColumns;
            for (int i = 0; i < labels; ++i)
            {
                const float x = (i % 4 - 1.5f) * 2.5f, z = (i / 4) * 3.3f;
                const auto clip = projection * view * FVector4{x, .2f, z - 1.2f, 1};
                if (clip.w <= 0)
                    continue;
                const float px = (clip.x / clip.w * .5f + .5f) * width,
                            py = (.5f - clip.y / clip.w * .5f) * height;
                ui.drawRoundedRect({px - 45, py - 2, px + 45, py + 21}, {.015f, .025f, .045f, .92f}, 3);
                const std::string label = std::to_string(i + 1) + "  " + ids[i];
                ui.drawText({px - 37, py, px + 44, py + 22}, std::wstring(label.begin(), label.end()), 12,
                            FVector4{.85f, .93f, 1, 1});
            }
        }
        if (sceneIndex == 3)
        {
            const auto view = lh::lookAt(exhibit.cameraPosition, exhibit.cameraTarget, {0, 1, 0});
            const auto projection = lh::perspective(radians(exhibit.fov), float(width) / height, .1f, 100);
            for (size_t i = 0; i < demo::kMaterialStudies.size(); ++i)
            {
                const float x = (int(i % 4) - 1.5f) * 2.6f, z = int(i / 4) * 3.2f;
                const auto clip = projection * view * FVector4{x, .35f, z - 1.25f, 1};
                if (clip.w <= 0)
                    continue;
                const float px = (clip.x / clip.w * .5f + .5f) * width,
                            py = (.5f - clip.y / clip.w * .5f) * height;
                ui.drawRoundedRect({px - 64, py - 2, px + 64, py + 22}, {.015f, .025f, .045f, .92f}, 3);
                const std::string label = demo::kMaterialStudies[i].label;
                ui.drawText({px - 56, py, px + 63, py + 22}, std::wstring(label.begin(), label.end()), 13,
                            FVector4{.85f, .93f, 1, 1});
            }
        }
        if (sceneIndex == 4)
        {
            const auto view = lh::lookAt(exhibit.cameraPosition, exhibit.cameraTarget, {0, 1, 0});
            const auto projection = lh::perspective(radians(exhibit.fov), float(width) / height, .1f, 100);
            for (size_t i = 0; i < demo::kMaterialCutoutLabels.size(); ++i)
            {
                const auto clip = projection * view * FVector4{(int(i) - 1) * 3.f, .4f, -.5f, 1};
                if (clip.w <= 0)
                    continue;
                const float px = (clip.x / clip.w * .5f + .5f) * width,
                            py = (.5f - clip.y / clip.w * .5f) * height;
                ui.drawRoundedRect({px - 80, py - 2, px + 80, py + 22}, {.015f, .025f, .045f, .92f}, 3);
                const std::string label = demo::kMaterialCutoutLabels[i];
                ui.drawText({px - 72, py, px + 79, py + 22}, std::wstring(label.begin(), label.end()), 13,
                            FVector4{.85f, .93f, 1, 1});
            }
        }
        ui.endCanvas();
        ui.endFrame();
        if (batch ? (motion ? local >= 4 && local % 3 == 1 : local == 22)
                  : GetForegroundWindow() == window && pressed(VK_F9))
        {
            const std::string name = (deferred ? std::string("deferred-") : std::string("forward-")) +
                                     (hero >= 0 ? ids[hero] : scenes[sceneIndex]) +
                                     (variant ? std::string("-") + variants[variant] : "") +
                                     (motion ? "-" + std::to_string(local) : "") + ".png";
            const auto file = output / name;
            expected.push_back(file);
            if (!renderer.captureScreenshot(utf8(file)))
                ++failures;
            std::printf("Capture %s\n", name.c_str());
        }
        renderer.endFrame();
        if (batch && local > 8 && !renderer.getFrameStats().graph.compileSucceeded)
            ++failures;
        ++tick;
        const std::string title = std::string("PBR Materials | ") +
                                  (deferred ? "Deferred | " : "Forward | ") +
                                  (hero >= 0 ? ids[hero] : scenes[sceneIndex]) + " | " + variants[variant] +
                                  " | Tab scene, M material, V variant, A animate, F path, F9 capture";
        SetWindowTextA(window, title.c_str());
    }
    for (auto m : ownedMaterials)
        renderer.destroyMaterial(m);
    for (auto m : {cube, sphere, cylinder, torus})
        renderer.destroyMesh(m);
    ui.shutdown();
    renderer.shutdown();
    DestroyWindow(window);
    for (const auto &file : expected)
        if (!fs::exists(file))
            ++failures;
    std::printf("Material gallery completed, failures=%d\n", failures);
    return failures ? 1 : 0;
}
int wmain(int argc, wchar_t **argv)
{
    ayt::render::demo::Utf8Arguments args(argc, argv);
    return runMaterials(argc, args.pointers.data());
}
