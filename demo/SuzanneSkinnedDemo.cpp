// SuzanneSkinnedDemo.cpp — Phase 1 GPU skinning end-to-end demo (Win32).
//
// Goal: prove the full RD-03 / RD-04 / SC-01 / AN-03 / E-01..E-04 chain
// works. Builds a 4-bone skeleton, a 24-vertex skinned cube (one face per
// bone, weight 1.0 per vertex), and a 2-second animation that lifts the
// "spine" bone along Y. Runs the engine for ~60 frames and captures
// screenshots at frame 10 / 30 / 60 to disk; the three images must differ,
// proving skin matrices drive the cube's mesh into a new position over
// time.
//
// Asset layout (all under <temp>\\ayengine_skinned\\):
//   - cube.aymesh        24 verts, 36 idx, with bone weights
//   - cube.aymat         material that drives the SkinnedLit phoskia
//   - suzanne.ayskel     4-bone skeleton (root, hip, spine, head)
//   - suzanne.ayanm      2-sec anim, 1 track on "spine" bone
//
// Failure modes to look for in the screenshots:
//   1. All faces clamped at origin      -> Skeleton UBO never uploaded
//   2. Cube whole-rigid-rotates         -> skinningMatrix not expanded
//   3. Three identical images           -> AnimationSystem never ticked
//   4. Black/empty image                -> SkinnedLit material failed compile
//
// Output:
//   <assetRoot>\\frame_10.png
//   <assetRoot>\\frame_30.png
//   <assetRoot>\\frame_60.png

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
#include "AYWorld.h"
#include "AYGameLoop.h"
#include "AYRendererSubSystem.h"
#include "AYMathUtils.h"

#include "components/AYAnimationComponent.h"
#include "components/AYMeshComponent.h"
#include "components/AYSkeletonComponent.h"

#include "assetsImpl/AYMaterial.h"
#include "assetsImpl/AYMesh.h"
#include "assetsImpl/AYSkeleton.h"
#include "assetsImpl/AYAnimation.h"

#include "AYFile.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace
{

constexpr int kWindowWidth    = 800;
constexpr int kWindowHeight   = 600;
constexpr int kMaxFrames      = 65;
constexpr int kWarmupFrames   = 12;
constexpr int kScreenshotAt[] = {10, 30, 60};

// The skinned cube's vertex layout (from AYResource::Mesh::createCube):
//   6 faces x 4 verts = 24 verts (duplicate positions at every edge/corner).
// Duplicate corners MUST share the same bone or they split apart during
// skinning (looks like one corner of the top face peeling up).
//
// Demo binding: bottom -Y face anchored to root[0]; all other faces to
// spine[2] so the upper shell lifts as one rigid piece when the spine
// track plays. Hinge seam is the horizontal ring at y = -h only.
//
//   vertex 0..3   = +X face  -> bone[2] (spine)
//   vertex 4..7   = -X face  -> bone[2] (spine)
//   vertex 8..11  = +Y face  -> bone[2] (spine)
//   vertex 12..15 = -Y face  -> bone[0] (root)
//   vertex 16..19 = +Z face  -> bone[2] (spine)
//   vertex 20..23 = -Z face  -> bone[2] (spine)
static const uint8_t kVertexBoneAssignment[24] = {
    2, 2, 2, 2,    // +X
    2, 2, 2, 2,    // -X
    2, 2, 2, 2,    // +Y
    0, 0, 0, 0,    // -Y
    2, 2, 2, 2,    // +Z
    2, 2, 2, 2,    // -Z
};

struct DemoAssets
{
    std::string assetDir;
    std::string meshPath;
    std::string materialPath;
    std::string skeletonPath;
    std::string animationPath;
};

struct DemoState
{
    ayt::game::GameLoop* loop = nullptr;
    bool running = true;
    int  frame   = 0;
    std::string screenshotDir;
};

bool writeBytes(const std::string& path, const void* data, size_t size)
{
    ayt::io::File file(path, ayt::io::File::Mode::BinaryWrite);
    if (!file.isOpen()) {
        return false;
    }
    return file.write(data, size) == size;
}

bool ensureDirectory(const std::string& path)
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

// Compute bind-pose world matrices and store inverse as IBM.
// Demo mesh vertices are authored in bind/model space; at rest pose
// skinMatrix must be identity or weighted vertices drift (e.g. +Y face
// bound to spine looks oversized / detached).
void computeInverseBindFromRestPose(ayt::resource::Bone bones[], int count)
{
    using namespace ayt::math;
    Float4x4 bindWorld[4];
    for (int i = 0; i < count; ++i) {
        const Float4x4 local = Float4x4::fromTRS(
            bones[i].localPosition,
            bones[i].localRotation,
            bones[i].localScale);
        if (bones[i].parentIndex < 0) {
            bindWorld[i] = local;
        } else {
            bindWorld[i] = bindWorld[bones[i].parentIndex] * local;
        }
        bones[i].inverseBindMatrix = bindWorld[i].inverse();
    }
}

// Build a 4-bone skeleton with parent-before-child order (skinningMatrix
// will walk this order at evaluate time).
void buildSkeleton(ayt::resource::Skeleton& skel)
{
    using namespace ayt::resource;
    skel.setBoneCount(4);

    Bone bones[4] = {};
    // root
    bones[0].name = "root";
    bones[0].parentIndex = -1;
    bones[0].localPosition  = ayt::math::FVector3(0, 0, 0);
    bones[0].localRotation  = ayt::math::FQuaternion::identity();
    bones[0].localScale     = ayt::math::FVector3(1, 1, 1);
    // hip
    bones[1].name = "hip";
    bones[1].parentIndex = 0;
    bones[1].localPosition  = ayt::math::FVector3(0, 0.25f, 0);
    bones[1].localRotation  = ayt::math::FQuaternion::identity();
    bones[1].localScale     = ayt::math::FVector3(1, 1, 1);
    // spine
    bones[2].name = "spine";
    bones[2].parentIndex = 1;
    bones[2].localPosition  = ayt::math::FVector3(0, 0.25f, 0);
    bones[2].localRotation  = ayt::math::FQuaternion::identity();
    bones[2].localScale     = ayt::math::FVector3(1, 1, 1);
    // head
    bones[3].name = "head";
    bones[3].parentIndex = 2;
    bones[3].localPosition  = ayt::math::FVector3(0, 0.25f, 0);
    bones[3].localRotation  = ayt::math::FQuaternion::identity();
    bones[3].localScale     = ayt::math::FVector3(1, 1, 1);

    computeInverseBindFromRestPose(bones, 4);

    for (int i = 0; i < 4; ++i) {
        skel.setBone(static_cast<size_t>(i), bones[i]);
    }
}

// Build a 2-second clip: spine local Y lift (looping).
// Keyframe times are in ticks (tps=30 -> 0/1/2 seconds).
// No root rotation — mixed root/spine corner bindings tear duplicate verts.
void buildAnimation(ayt::resource::Animation& anim)
{
    using namespace ayt::resource;
    anim.setName("skinned_demo_anim");
    anim.setTicksPerSecond(30.0f);
    anim.setDuration(2.0f);

    AnimTrack track;
    track.nodeName = "spine";
    track.property = "position";
    track.valueType = AnimTrackType::Vector3;
    track.times  = {0.0f, 30.0f, 60.0f};
    // Bind-pose spine local Y is 0.25; lift to 0.65 then return.
    track.values = {
        0.0f, 0.25f, 0.0f,
        0.0f, 0.65f, 0.0f,
        0.0f, 0.25f, 0.0f,
    };
    anim.addTrack(track);
}

// Build a 24-vertex cube with per-vertex single-bone skin weights (1.0
// on the assigned bone, 0 elsewhere — we always pad to 4 channels).
bool buildSkinnedMesh(ayt::resource::Mesh& mesh)
{
    mesh.createCube(1.0f);  // 24 verts, 36 idx, pos+norm+uv mask

    std::vector<ayt::resource::VertexSkinWeight> weights(24);
    for (uint32_t v = 0; v < 24; ++v) {
        const uint8_t bone = kVertexBoneAssignment[v];
        weights[v].boneIndex[0] = bone;
        weights[v].boneIndex[1] = 0;
        weights[v].boneIndex[2] = 0;
        weights[v].boneIndex[3] = 0;
        weights[v].boneWeight[0] = 1.0f;
        weights[v].boneWeight[1] = 0.0f;
        weights[v].boneWeight[2] = 0.0f;
        weights[v].boneWeight[3] = 0.0f;
    }
    mesh.debugSetSkinWeights(weights);
    return mesh.hasSkinWeights() && mesh.getSkinWeights() != nullptr;
}

bool bakeDemoAssets(const std::string& rootPrefix, DemoAssets& out)
{
    if (!ensureDirectory(rootPrefix)) {
        return false;
    }
    out.assetDir      = rootPrefix + "assets\\";
    out.meshPath      = out.assetDir + "cube.aymesh";
    out.materialPath  = out.assetDir + "cube.aymat";
    out.skeletonPath  = out.assetDir + "suzanne.ayskel";
    out.animationPath = out.assetDir + "suzanne.ayanm";
    if (!ensureDirectory(out.assetDir)) {
        return false;
    }

    // Mesh (with skin weights)
    {
        ayt::resource::Mesh mesh;
        if (!buildSkinnedMesh(mesh)) {
            std::fprintf(stderr, "[SkinnedDemo] buildSkinnedMesh failed\n");
            return false;
        }
        std::vector<ayt::resource::UInt8> bin;
        if (!mesh.saveToBinary(bin)) {
            std::fprintf(stderr, "[SkinnedDemo] mesh.saveToBinary failed\n");
            return false;
        }
        if (!writeBytes(out.meshPath, bin.data(), bin.size())) {
            return false;
        }
    }

    // Material — the SkinnedMeshRenderSystem actually compiles the
    // SkinnedLit Phoskia source INLINE so this .aymat is only used to
    // (optionally) be a debug artifact. We bake a minimal material
    // with a single textureless reference so the file exists.
    {
        ayt::resource::Material mat;
        mat.setShader("skinned_lit.phoskia");
        const ayt::resource::Float32 baseColor[4] = {1.0f, 0.85f, 0.6f, 1.0f};
        mat.setFloat4("baseColor", baseColor);
        const ayt::resource::Float32 lightDir[3]  = {-0.4f, -0.8f, 0.45f};
        mat.setFloat3("lightDir", lightDir);
        const ayt::resource::Float32 lightColor[3] = {1.0f, 0.95f, 0.85f};
        mat.setFloat3("lightColor", lightColor);
        std::vector<ayt::resource::UInt8> bin;
        if (!mat.saveToBinary(bin)) {
            return false;
        }
        if (!writeBytes(out.materialPath, bin.data(), bin.size())) {
            return false;
        }
    }

    // Skeleton
    {
        ayt::resource::Skeleton skel;
        buildSkeleton(skel);
        std::vector<ayt::resource::UInt8> bin;
        if (!skel.saveToBinary(bin)) {
            std::fprintf(stderr, "[SkinnedDemo] skeleton.saveToBinary failed\n");
            return false;
        }
        if (!writeBytes(out.skeletonPath, bin.data(), bin.size())) {
            return false;
        }
    }

    // Animation
    {
        ayt::resource::Animation anim;
        buildAnimation(anim);
        std::vector<ayt::resource::UInt8> bin;
        if (!anim.saveToBinary(bin)) {
            std::fprintf(stderr, "[SkinnedDemo] animation.saveToBinary failed\n");
            return false;
        }
        if (!writeBytes(out.animationPath, bin.data(), bin.size())) {
            return false;
        }
    }

    std::fprintf(stderr, "[SkinnedDemo] baked assets in %s\n", out.assetDir.c_str());
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
    wc.lpszClassName = L"AYSuzanneSkinnedDemo";
    RegisterClassExW(&wc);

    RECT rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"AY Suzanne Skinned Demo (Phase 1 mid-point)",
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

bool shouldScreenshotThisFrame(int frame)
{
    for (int f : kScreenshotAt) {
        if (f == frame) return true;
    }
    return false;
}

ayt::entity::Entity* spawnSkinnedEntity(const DemoAssets& assets)
{
    ayt::entity::Entity* entity = ayt::entity::World::instance().createEntity();
    if (entity == nullptr) {
        return nullptr;
    }

    entity->addComponent<ayt::entity::Transform>();

    auto* mesh = entity->addComponent<ayt::entity::MeshComponent>();
    mesh->meshPath     = assets.meshPath;
    mesh->materialPath = assets.materialPath;
    mesh->skinned      = true;

    auto* skel = entity->addComponent<ayt::entity::SkeletonComponent>();
    skel->skeletonPath = assets.skeletonPath;

    auto* anim = entity->addComponent<ayt::entity::AnimationComponent>();
    anim->clipPath = assets.animationPath;
    anim->autoplay = true;
    anim->looping  = true;
    anim->playRate = 1.0f;

    std::fprintf(stderr,
                 "[SkinnedDemo] spawned ECS skinned entity (skel=%s, clip=%s)\n",
                 assets.skeletonPath.c_str(), assets.animationPath.c_str());
    return entity;
}

bool prepareDemoWorld()
{
    ayt::entity::bootstrapModule();
    if (!ayt::entity::World::instance().initialize()) {
        std::fprintf(stderr, "[SkinnedDemo] World::initialize failed\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    // Phase 1 demo is two-phase to separate slow first-time shader
    // compile from the fast playback run:
    //
    //   --warmup   : run 12 frames to load skeleton, upload mesh, compile
    //                SkinnedLit, and populate shader_cache/.
    //
    //   (default)  : run 65 frames; capture frame_10/30/60.png after
    //                warmup has populated the shader cache.
    //
    // The cache directory is <assetRoot>/shader_dump/ and is the
    // same path on both runs, so the second run picks up the .sc
    // sources dumped by the first.
    bool warmupMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warmup") == 0) {
            warmupMode = true;
        }
    }

    DemoState state;
    ayt::game::GameLoop& loop = ayt::game::GameLoop::instance();
    state.loop = &loop;

    HWND hwnd = createDemoWindow(&state);
    if (hwnd == nullptr) {
        std::fprintf(stderr, "[SkinnedDemo] failed to create Win32 window\n");
        return 1;
    }

    ayt::render::RendererSubSystem::setBootstrapWindow(
        hwnd,
        static_cast<uint32_t>(kWindowWidth),
        static_cast<uint32_t>(kWindowHeight));
    ayt::render::RendererSubSystem::setBootstrapViewport(
        0, 0,
        static_cast<uint16_t>(kWindowWidth),
        static_cast<uint16_t>(kWindowHeight));

    char tempDir[MAX_PATH] = {};
    std::string assetRoot = "skinned_demo_assets\\";
    if (GetTempPathA(MAX_PATH, tempDir) > 0) {
        assetRoot = std::string(tempDir) + "ayengine_skinned\\";
    }
    state.screenshotDir = assetRoot;

    DemoAssets assets;
    if (!bakeDemoAssets(assetRoot, assets)) {
        std::fprintf(stderr, "[SkinnedDemo] bakeDemoAssets failed\n");
        return 1;
    }

    std::fprintf(stderr, "[SkinnedDemo] mesh     = %s\n", assets.meshPath.c_str());
    std::fprintf(stderr, "[SkinnedDemo] material = %s\n", assets.materialPath.c_str());
    std::fprintf(stderr, "[SkinnedDemo] skeleton = %s\n", assets.skeletonPath.c_str());
    std::fprintf(stderr, "[SkinnedDemo] anim     = %s\n", assets.animationPath.c_str());
    std::fprintf(stderr, "[SkinnedDemo] shaderc hint: %s\n", AY_SHADER_SHADERC_HINT);

    // Tell the shader pool to dump .sc sources (post-Phoskia emit) to
    // disk so subsequent runs can audit / pre-load them.
    const std::string shaderDumpDir = assetRoot + "shader_dump\\";
    if (ensureDirectory(shaderDumpDir)) {
        ayt::render::RendererSubSystem::setBootstrapShaderDumpDirectory(shaderDumpDir);
        std::fprintf(stderr, "[SkinnedDemo] shader dump dir: %s\n", shaderDumpDir.c_str());
    }

    const std::string shaderCacheDir = assetRoot + "shader_cache\\";
    if (ensureDirectory(shaderCacheDir)) {
        ayt::render::RendererSubSystem::setBootstrapShaderCacheDirectory(shaderCacheDir);
        std::fprintf(stderr, "[SkinnedDemo] shader cache dir: %s\n", shaderCacheDir.c_str());
    }

    if (!prepareDemoWorld()) {
        return 1;
    }
    if (spawnSkinnedEntity(assets) == nullptr) {
        std::fprintf(stderr, "[SkinnedDemo] spawnSkinnedEntity failed\n");
        return 1;
    }

    loop.setTargetFPS(60.0f);
    loop.setRenderThreadEnabled(false);

    if (warmupMode) {
        // --warmup: run several frames so skeleton loads, mesh uploads,
        // and SkinnedLit compiles before the timed screenshot run.
        std::fprintf(stderr,
                     "[SkinnedDemo] --warmup: running %d frames to compile shaders\n",
                     kWarmupFrames);
        int warmupFrame = 0;
        const uint64_t warmupListenerId = loop.onUpdate([&](float /*dt*/) {
            pumpWin32Messages(state);
            if (++warmupFrame >= kWarmupFrames) {
                std::fprintf(stderr,
                             "[SkinnedDemo] --warmup: done after %d frames; exiting\n",
                             warmupFrame);
                state.running = false;
                loop.stop();
            }
        });
        loop.run();
        loop.offUpdate(warmupListenerId);
        loop.shutdown();
        std::printf("AYSuzanneSkinned_Demo warmup finished.\n");
        return 0;
    }

    const uint64_t listenerId = loop.onUpdate([&](float /*deltaTime*/) {
        pumpWin32Messages(state);

        if (!state.running) {
            loop.stop();
            return;
        }

        // Screenshot at the configured moments.
        if (shouldScreenshotThisFrame(state.frame)) {
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%02d", state.frame);
            const std::string base = state.screenshotDir + name;
            ayt::render::RendererSubSystem* rss =
                ayt::render::RendererSubSystem::findRegistered();
            if (rss != nullptr) {
                if (rss->renderer().captureScreenshot(base)) {
                    std::fprintf(stderr,
                                 "[SkinnedDemo] frame %d -> screenshot %s (.tga/.png)\n",
                                 state.frame, base.c_str());
                } else {
                    std::fprintf(stderr,
                                 "[SkinnedDemo] frame %d captureScreenshot failed\n",
                                 state.frame);
                }
            }
        }

        // Force-stop after kMaxFrames frames.
        if (state.frame >= kMaxFrames) {
            std::fprintf(stderr,
                         "[SkinnedDemo] reached %d frames; stopping loop\n",
                         kMaxFrames);
            state.running = false;
            loop.stop();
            return;
        }
        state.frame++;
    });

    loop.run();
    loop.offUpdate(listenerId);
    loop.shutdown();

    std::fprintf(stderr, "[SkinnedDemo] done. frames=%d\n", state.frame);
    std::printf("AYSuzanneSkinned_Demo finished.\n");
    return 0;
}
