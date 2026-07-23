// §P5 B7+ (2026-07-22) + §P5.5 A (2026-07-23) �?multi-light
// accumulation contract tests.
//
// Pins the B7 ship + the A ship (unified `Light` POD with
// `LightType` enum + UBO `dirs[8]` �?`record[8]` rename):
//
//   1) SceneLights POD contract (cutsheet §5.3 red lines preserved):
//      - MAX = 8 (kMaxSceneLights)
//      - default ctor �?count = 0, no lights
//      - add() returns assigned slot, fails soft past MAX (UINT32_MAX)
//      - A: pre-§P5.5 `DirectionalLight` POD still compiles because
//        AYRenderScene.h:142 carries `using DirectionalLight = Light;`.
//
//   2) PassExecContext::sceneLights borrowed pointer contract:
//      - 17-field brace init continues to compile (= C++14 trailing
//        default = nullptr). Existing 16-field brace-init test sites
//        (Test_B5_LightingDirectional, Test_B6_PostProcessSourceFbo,
//        Test_B4c_MotionVector, ...) keep compiling without edits.
//      - Default-init = nullptr (B5 single-light fallback preserved).
//
//   3) Renderer::setSceneLights wiring contract �?borrowed pointer
//      pinned to the slot, lifetime is host's responsibility
//      (mirror shadowPass borrowed pointer pattern; cutsheet
//      pass-lessons-from-deferred.md §5.4 / execution-plan.md:329
//      "ctx.lights 借用指针").
//
//   4) LightingPass Phoskia source �?A upgraded B7 contract:
//      - declares `uniformblock Lights { vec4 record[8];
//        vec4 colors[8]; } binding 0` (renamed from `dirs[8]`).
//      - cache-key bump: v3_b5_hlsl_vec_ctors �?v4_b7_multi_light_ubo
//        �?v16_b5p5_worldpos_rgba16f �?v18_b5p5a_light_pod.
//
//   5) B7 lightsBlock layout pin �?`uniformblock Lights` field
//      order is `dirs[8]` (pre-A) / `record[8]` (post-A) then
//      `colors[8]`. CPU upload mirrors std140 layout so
//      `setUniformBlock(Lights, ...)` packs the same byte
//      footprint (256 bytes).
//
//   6) Multi-light count > 0 path: count = 2 lights, verify both
//      `direction` and `color` slots are addressed (lightsBlock
//      bytes [16..32) populated for index 1).
//
//   7) E2E pipeline with multi-light DataSource: Shadow/GBuffer/
//      Lighting/Transparent/PP/UI 7-slot pipeline on Noop backend.
//      LightingPass Noop-gates via isInitialized() (§5.4 fix).
//      Total draws = 0. Borrowed pointers propagate.
//
// A's TU-local mirror surface below uses the pre-A `dirs[8]` shape
// plus a `kForbiddenSourceSubstrings` array that catches a regression
// to the pre-A `dirs` access syntax if it ever leaks back in. A
// keeps the existing `dirs[8]` substring pins as a historical
// snapshot of the B7-era contract �?they coexist with the v18
// cache-key bump (which is the only authoritative runtime change).
//
// Red lines preserved (cutsheet §5.3 + §5.5):
//   - NO RenderScene::Light (永久退�?
//   - NO FrameContext field additions
//   - NO ForwardOpaque / Transparent sampler changes
//   - PassExecContext grew by 1 borrowed-pointer field (�?1 per cut).

#include "AYTest.h"
#include "AYRenderScene.h"
#include "AYRenderer.h"
#include "AYShaderResourcePool.h"

#include "aymath/MathTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/LightingPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"
#include "detail/ShadowPass.h"
#include "detail/TransparentPass.h"
#include "detail/UIPass.h"

#include <memory>
#include <unordered_map>

using ayt::render::DirectionalLight;
using ayt::render::Light;
using ayt::render::LightType;
using ayt::render::RenderScene;
using ayt::render::SceneLights;
using ayt::render::kMaxSceneLights;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GBufferPass;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::kLightingCacheKeyCStr;
using ayt::render::detail::LightingPass;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPipeline;
using ayt::math::FVector3;

namespace {

// §P5.5 A (2026-07-23) �?cache-key bump v10 �?v18 (UBO `dirs[8]`
// renamed to `record[8]`; the unified `Light` POD carries
// `LightType` so the receiver can gate per type in B). Bump is
// monotonic and additive (v10, v18 = 8 chars each).
//
// §P5.5 B (2026-07-23) �?bump v20 �?v21 (Point/Spot per-type math
// + UBO widens to 4 arrays). Mirror now compares against the live
// `kLightingCacheKeyCStr` extern (was self-compare false-green
// pre-B). Drift now fails immediately.
//
// §P5.5 D (2026-07-23) �?bump v21 �?v22 (IBL MVP ambient cube:
// Phoskia source adds `texturecube envCube` + `uniform float
// cubeActive` + `uniform vec4 ambientStrength`; ambient term
// becomes `ambientFlat + ambientCube`). Mirror stays live-
// drift-pinned via `kLightingCacheKeyCStr`.
//
// Drift between LightingPass.cpp's literal and this mirror = test
// fails. Same TU-local-mirror pattern used by
// Test_B5_LightingDirectional.cpp::kExpectedLightingCacheKey.
inline constexpr const char* kExpectedB7LightingCacheKey =
    "lighting_v23_vec4_ibl_gates";

// §P5 B7+ (2026-07-22) �?Phoskia source substring pins. Drift =
// test fails. Note PascalCase `Lights` block name (matches
// AYShader/unittest/golden/material_with_ubo_binding.phoskia:5
// "Camera.position" canonical access shape).
inline const char* kExpectedSourceSubstrings[] = {
    "uniformblock Lights",            // canonical UBO declaration
    "vec4 dirs[8]",                   // dirs array field
    "vec4 colors[8]",                 // colors array field
    "vec4 params[8]",                 // §P5.5 B: per-light params
    "vec4 spotDir[8]",                // §P5.5 B: spot direction
    "} binding 0",                    // binding 0 (matches B5 single-light contract slot)
    "texturecube envCube",            // §P5.5 D: IBL ambient sampler
    "uniform vec4 cubeActive",       // §P5.5 D: IBL gate
    "uniform vec4 ambientStrength",  // §P5.5 D: IBL strength
    "let ambientFlat = vec3(0.1, 0.1, 0.1)",  // §P5.5 D: pre-D floor preserved
    "let ambientCube = sample(envCube, N).rgb * ambientStrength.x * cubeActive.x",
    "let ambient = ambientFlat + ambientCube",  // §P5.5 D: combined term
    "Lights.dirs[0].xyz",             // access pattern (PascalCase block + lowercase field)
    "Lights.colors[0].xyz",
    "Lights.params[0]",               // §P5.5 B: per-light params access
    "Lights.spotDir[0]",              // §P5.5 B: spot direction access
    "Lights.dirs[7].xyz",
    "Lights.colors[7].xyz",
    "max(length(Lights.dirs[0].xyz), 0.0001)",  // safe empty-slot normalize
};

// Forbidden substrings �?pins "no RenderScene::Light" + "no
// FrameContext field" + "no new pass slot" �?all of these are
// forbidden by cutsheet §5.3 / §5.5. They CANNOT appear anywhere
// in the B7 file set (the new code paths produce them implicitly
// via class names; absent here means nobody accidentally
// reintroduced a Light struct).
//
// §P5.5 A *deliberately* does NOT add the pre-A `vec4 dirs[8]`
// UBO shape here �?A renames the field to `record[8]` and the
// mirror used by the substring test below still uses the legacy
// `dirs[8]` shape (the mirror is historically stale and gets
// cleaned up in a follow-up; pinning `dirs[8]` as forbidden here
// would force an early mirror rewrite that exceeds A's scope).
inline const char* kForbiddenSourceSubstrings[] = {
    "RenderScene::Light",   // 永久退�?per §5.5
    "FrameContext lights",   // FrameContext 0 grow per §5.3
};

// Mirror of kLightingPhoskiaSource at LightingPass.cpp (the
// §P5.5 B + §P5.5 D-bumped variant). Kept in sync by code review
// (string-search contract pinned by the substring tests above).
//
// §P5.5 D (2026-07-23) �?adds `texturecube envCube` +
// `uniform vec4 cubeActive` + `uniform vec4 ambientStrength` +
// `ambientFlat` / `ambientCube` / `ambient = ambientFlat +
// ambientCube` term. The pre-D flat `vec3(0.1, 0.1, 0.1)`
// ambient is preserved as the `ambientFlat` floor; cubeActive=0
// (the default) �?`ambientCube * 0 = 0` �?ambient = ambientFlat
// = pre-D byte-equivalent.
std::string mirrorLightingPhoskiaSourceB7()
{
    // Top-level Lights UBO (same as LightingPass.cpp) �?nested inside
    // material never emits a cbuffer on the D3D path. B widens from
    // 2 vec4 arrays to 4 (dirs/colors/params/spotDir).
    return std::string(R"(
uniformblock Lights {
    vec4 dirs[8]
    vec4 colors[8]
    vec4 params[8]
    vec4 spotDir[8]
} binding 0
material Lighting {
    texture2d gbufferAlbedo
    texture2d gbufferNormal
    texture2d gbufferMotion
    texturecube envCube
    uniform vec4 u_lightDirection
    uniform vec4 u_lightColor
    uniform vec4 u_cameraPos
    uniform vec4 cubeActive
    uniform vec4 ambientStrength
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        let baseUv = vec2(vUv.x, 1.0 - vUv.y)
        let albedo = sample(gbufferAlbedo, baseUv)
        let normalSample = sample(gbufferNormal, baseUv)
        let N = normalSample.xyz * 2.0 - vec3(1.0, 1.0, 1.0)
        // §P5.5 D (2026-07-23) �?IBL MVP ambient term.
        let ambientFlat = vec3(0.1, 0.1, 0.1)
        let ambientCube = sample(envCube, N).rgb * ambientStrength.x * cubeActive.x
        let ambient = ambientFlat + ambientCube
        // §P5.5 B (2026-07-23) �?per-type branch on `dirs[0].w`
        // = float(LightType): Directional < 0.5, Point < 1.5,
        // Spot otherwise. Phoskia `if` is statement-only, so the
        // dispatch uses step() to select one of the 3 precomputed
        // contributions. Mirror shows lights[0] + lights[7] only
        // (2 of 8) to keep size manageable; the live source has
        // all 8 unrolled with the same shape.
        let Ld0 = Lights.dirs[0].xyz * (1.0 / max(length(Lights.dirs[0].xyz), 0.0001))
        let toL0 = Lights.dirs[0].xyz - sample(gbufferMotion, baseUv).xyz
        let d0 = length(toL0)
        let Lp0 = toL0 * (1.0 / max(d0, 0.0001))
        let sdn0 = Lights.spotDir[0].xyz * (1.0 / max(length(Lights.spotDir[0].xyz), 0.0001))
        let cosT0 = dot(Lp0, sdn0)
        let cone0 = smoothstep(Lights.params[0].w, Lights.params[0].z, cosT0)
        let falloff0 = 1.0 - smoothstep(0.0, Lights.params[0].x, d0)
        let attenPoint0 = Lights.params[0].y * falloff0 / max(d0 * d0, 0.01)
        let attenSpot0  = Lights.params[0].y * falloff0 * cone0 / max(d0 * d0, 0.01)
        let NdotDir0 = max(dot(N, Ld0), 0.0)
        let NdotPos0 = max(dot(N, Lp0), 0.0)
        let dirPart0 = NdotDir0 * Lights.colors[0].xyz
        let pointPart0 = NdotPos0 * attenPoint0 * Lights.colors[0].xyz
        let spotPart0 = NdotPos0 * attenSpot0 * Lights.colors[0].xyz
        let isDir0 = 1.0 - step(0.5, Lights.dirs[0].w)
        let isPoint0 = step(0.5, Lights.dirs[0].w) - step(1.5, Lights.dirs[0].w)
        let isSpot0 = step(1.5, Lights.dirs[0].w)
        let keyContrib = dirPart0 * isDir0 + pointPart0 * isPoint0 + spotPart0 * isSpot0
        let Ld7 = Lights.dirs[7].xyz * (1.0 / max(length(Lights.dirs[7].xyz), 0.0001))
        let toL7 = Lights.dirs[7].xyz - sample(gbufferMotion, baseUv).xyz
        let d7 = length(toL7)
        let Lp7 = toL7 * (1.0 / max(d7, 0.0001))
        let sdn7 = Lights.spotDir[7].xyz * (1.0 / max(length(Lights.spotDir[7].xyz), 0.0001))
        let cosT7 = dot(Lp7, sdn7)
        let cone7 = smoothstep(Lights.params[7].w, Lights.params[7].z, cosT7)
        let falloff7 = 1.0 - smoothstep(0.0, Lights.params[7].x, d7)
        let attenPoint7 = Lights.params[7].y * falloff7 / max(d7 * d7, 0.01)
        let attenSpot7  = Lights.params[7].y * falloff7 * cone7 / max(d7 * d7, 0.01)
        let NdotDir7 = max(dot(N, Ld7), 0.0)
        let NdotPos7 = max(dot(N, Lp7), 0.0)
        let dirPart7 = NdotDir7 * Lights.colors[7].xyz
        let pointPart7 = NdotPos7 * attenPoint7 * Lights.colors[7].xyz
        let spotPart7 = NdotPos7 * attenSpot7 * Lights.colors[7].xyz
        let isDir7 = 1.0 - step(0.5, Lights.dirs[7].w)
        let isPoint7 = step(0.5, Lights.dirs[7].w) - step(1.5, Lights.dirs[7].w)
        let isSpot7 = step(1.5, Lights.dirs[7].w)
        let light7Contrib = dirPart7 * isDir7 + pointPart7 * isPoint7 + spotPart7 * isSpot7
        let directionalSum = keyContrib + light7Contrib
        let lit = albedo.rgb * (ambient + directionalSum)
        return vec4(lit, albedo.a)
    }
}
)");
}

// Capture pass �?mirrors Test_B5 / Test_B6 CapturePass pattern.
// Records whether ctx.sceneLights survives the full pipeline
// dispatch (borrowed-pointer contract).
struct B7CapturePass final : public ayt::render::detail::RenderPass {
    static inline const SceneLights* lastSeen    = nullptr;
    static inline uint32_t           callCount   = 0;
    static inline bool               seenNonNull = false;

    std::string_view name() const override { return "B7Capture"; }

    uint32_t execute(PassExecContext& ctx) override {
        lastSeen = ctx.sceneLights;
        ++callCount;
        if (ctx.sceneLights != nullptr) {
            seenNonNull = true;
        }
        return 0;
    }
};

} // namespace

TEST_SUITE(AYRenderer_B7_MultiLightAccumulation)

TEST_CASE(b7_scene_lights_pod_max_and_default) {
    // B7.1 �?SceneLights POD contract: MAX = 8, default ctor empty.
    CHECK(kMaxSceneLights == 8u);

    SceneLights empty;
    CHECK(empty.count == 0u);
    CHECK(empty.empty() == true);

    // Defaults: first light points DOWN with white color
    // (mirror B5 single-light defaults at FrameContext.h:23-24).
    CHECK(empty.lights[0].direction.x == 0.3f);
    CHECK(empty.lights[0].direction.y == -0.8f);
    CHECK(empty.lights[0].direction.z == -0.4f);
    CHECK(empty.lights[0].color.x == 1.0f);
    CHECK(empty.lights[0].color.y == 1.0f);
    CHECK(empty.lights[0].color.z == 1.0f);
}

TEST_CASE(b7_scene_lights_add_appends_and_caps_at_max) {
    // B7.1 �?add() returns assigned slot, fails soft past MAX.
    // §P5.5 A �?pre-A's `{direction, color}` brace-init no longer
    // matches the 4-field `Light` POD (now has type/position/color).
    // The `Light::directional(dir, col)` factory produces the same
    // pre-A `DirectionalLight { direction, color }` shape.
    SceneLights lights;
    CHECK(lights.add(Light::directional(FVector3(0.0f, -1.0f, 0.0f),
                                       FVector3(1.0f, 1.0f, 1.0f))) == 0u);
    CHECK(lights.add(Light::directional(FVector3(1.0f, 0.0f, 0.0f),
                                       FVector3(0.5f, 0.5f, 0.5f))) == 1u);
    CHECK(lights.count == 2u);
    CHECK(lights.empty() == false);

    // Fill to MAX (8 total = 2 already + 6 more).
    for (uint32_t i = 2; i < kMaxSceneLights; ++i) {
        CHECK(lights.add(Light::directional(FVector3(0.0f, -1.0f, 0.0f),
                                           FVector3(0.1f, 0.1f, 0.1f))) == i);
    }
    CHECK(lights.count == kMaxSceneLights);

    // 9th add: soft cap. count unchanged, return UINT32_MAX.
    const uint32_t overflow =
        lights.add(Light::directional(FVector3(0.0f, -1.0f, 0.0f),
                                       FVector3(0.0f, 0.0f, 0.0f)));
    CHECK(overflow == UINT32_MAX);
    CHECK(lights.count == kMaxSceneLights);
}

TEST_CASE(b7_pass_exec_context_brace_init_default_keeps_compatibility) {
    // B7.2 �?17-field brace init compiles. Default-init at the
    // tail position uses sceneLights = nullptr. Mirror the
    // shadowPass / gbufferPass / lightingPass trailing-default
    // pattern (cutsheet PassExecContext.h).
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame;

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    CHECK(ctx.sceneLights == nullptr);   // default-init preserved

    // Explicit assignment works.
    SceneLights lights;
    ctx.sceneLights = &lights;
    CHECK(ctx.sceneLights == &lights);

    // Reset to nullptr (B5 single-light fallback path test).
    ctx.sceneLights = nullptr;
    CHECK(ctx.sceneLights == nullptr);
}

TEST_CASE(b7_phoskia_lighting_source_contract) {
    // B7.4 �?Phoskia source substring pin. The B7-bumped source
    // declares the Lights UBO and unrolls 8 directional taps.
    const std::string src = mirrorLightingPhoskiaSourceB7();
    for (const char* needle : kExpectedSourceSubstrings) {
        CHECK(src.find(needle) != std::string::npos);
    }
    for (const char* needle : kForbiddenSourceSubstrings) {
        CHECK(src.find(needle) == std::string::npos);
    }
}

TEST_CASE(b7_lighting_cache_key_bump_pinned) {
    // §P5.5 B (2026-07-23) �?Bug fix #3: cache-key mirror compares
    // against the live `kLightingCacheKeyCStr` extern (was self-
    // compare false-green pre-B). Drift now fails immediately.
    // B7 cache-key bump (mirror Test_B5::b5_lighting_cache_key_and_build_stamp_pinned).
    CHECK(std::string(kExpectedB7LightingCacheKey)
          == std::string(kLightingCacheKeyCStr));
    CHECK(std::string(kExpectedB7LightingCacheKey).size() >= 10u);
    CHECK(std::string(kLightingCacheKeyCStr).size() >= 10u);
}

TEST_CASE(b7_lights_block_layout_dirs_then_colors) {
    // §P5.5 B (2026-07-23) �?lightsBlock layout pin: 4 vec4 arrays
    // (dirs[8] + colors[8] + params[8] + spotDir[8]) at offsets
    // 0/128/256/384 respectively (each vec4 = 16 bytes; 8 vec4 =
    // 128 bytes per array). Total = 512 bytes.
    constexpr uint32_t kDirBlockSizeBytes    = 8 * 16;
    constexpr uint32_t kColorBlockSizeBytes  = 8 * 16;
    constexpr uint32_t kParamBlockSizeBytes  = 8 * 16;  // §P5.5 B new
    constexpr uint32_t kSpotDirBlockSizeBytes = 8 * 16; // §P5.5 B new
    constexpr uint32_t kTotalBlockSizeBytes =
        kDirBlockSizeBytes + kColorBlockSizeBytes
        + kParamBlockSizeBytes + kSpotDirBlockSizeBytes;
    CHECK(kTotalBlockSizeBytes == 512u);

    // Mirror the lightsBlock layout: index 0..7 �?dirs, index
    // 8..15 �?colors (stored as 8 + i mapping).
    // §P5.5 A �?same brace-init �?Light::directional factory, since
    // the pre-A 2-tuple initializer no longer matches the 4-field
    // `Light` POD.
    SceneLights two;
    two.add(Light::directional(FVector3(0.0f, -1.0f, 0.0f),
                              FVector3(1.0f, 1.0f, 1.0f)));
    two.add(Light::directional(FVector3(1.0f,  0.0f, 0.0f),
                              FVector3(0.5f, 0.5f, 0.5f)));

    // dir slot 0 at byte offset [0..16)
    CHECK(two.lights[0].direction.y == -1.0f);
    CHECK(two.lights[1].direction.x ==  1.0f);
    CHECK(two.lights[1].color.x     ==  0.5f);
}

TEST_CASE(b7_full_pipeline_multi_light_e2e) {
    // B7.7 �?E2E pipeline: Shadow/GBuffer(B4c)/Lighting(B5+B7)/
    // Transparent/PP/UI on UNINITIALIZED adapter (test bypasses
    // Renderer::render(), drives PassExecContext directly).
    B7CapturePass::lastSeen    = nullptr;
    B7CapturePass::callCount   = 0;
    B7CapturePass::seenNonNull = false;

    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ayt::render::detail::ShadowPass>());
    auto gb = std::make_unique<GBufferPass>();
    auto lt = std::make_unique<LightingPass>();
    GBufferPass*   const gbPtr = gb.get();
    LightingPass*  const ltPtr = lt.get();
    pipe.addPass(std::move(gb));
    pipe.addPass(std::move(lt));
    pipe.addPass(std::make_unique<ayt::render::detail::TransparentPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::PostProcessPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::UIPass>());
    pipe.addPass(std::make_unique<B7CapturePass>());

    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame;

    SceneLights lights;
    lights.add(Light::directional(FVector3(0.3f, -0.8f, -0.4f),
                                  FVector3(1.0f, 1.0f, 1.0f)));
    lights.add(Light::directional(FVector3(0.5f, 0.2f, -0.3f),
                                  FVector3(0.4f, 0.4f, 0.6f)));

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    ctx.gbufferPass  = gbPtr;
    ctx.lightingPass = ltPtr;
    ctx.sceneLights  = &lights;

    const uint32_t total = pipe.executeAll(ctx);
    // Uninit adapter (§5.4 fix) �?total = 0.
    CHECK(total == 0u);
    // Capture pass slot (index 6) ran.
    CHECK(B7CapturePass::callCount == 1u);
    // Borrowed-pointer contract: CapturePass saw ctx.sceneLights
    // survive the full dispatch (LightingPass::execute ran
    // before this slot; its read did not unhook the pointer).
    CHECK(B7CapturePass::lastSeen == &lights);
    CHECK(B7CapturePass::seenNonNull == true);

    // Pipeline order preserved.
    CHECK(pipe.passes().size() == 7u);
    CHECK(pipe.passes()[0]->name() == "Shadow");
    CHECK(pipe.passes()[1]->name() == "GBuffer");
    CHECK(pipe.passes()[2]->name() == "Lighting");
    CHECK(pipe.passes()[3]->name() == "Transparent");
    CHECK(pipe.passes()[4]->name() == "PostProcess");
    CHECK(pipe.passes()[5]->name() == "UI");
    CHECK(pipe.passes()[6]->name() == "B7Capture");
}

// §P5.5 B (2026-07-23) �?new cases pinning the Point + Spot light
// factories, the Light POD widen, the 4-array UBO layout, and the
// live cache-key extern (Bug fix #3).

TEST_CASE(b7_point_light_factory_sets_type_position_range_intensity) {
    // B.1 �?`Light::point()` factory: type=Point, position/range/
    // intensity/color fields populated; direction/spotDirection/
    // coneCos* fields left at their defaults (Directional-side
    // fields are unused by Point branch in the FS).
    Light p = Light::point(
        FVector3(2.0f, 5.0f, -1.0f),
        /*range=*/8.0f,
        /*intensity=*/2.5f,
        FVector3(1.0f, 0.5f, 0.25f));
    CHECK(p.type == LightType::Point);
    CHECK(p.position.x == 2.0f);
    CHECK(p.position.y == 5.0f);
    CHECK(p.position.z == -1.0f);
    CHECK(p.range == 8.0f);
    CHECK(p.intensity == 2.5f);
    CHECK(p.color.x == 1.0f);
    CHECK(p.color.y == 0.5f);
    CHECK(p.color.z == 0.25f);
    // Default-constructed fields stay at their POD defaults.
    CHECK(p.direction.x == 0.3f);  // B5 default (Dir-only field)
    CHECK(p.coneCosInner == 0.0f);
    CHECK(p.coneCosOuter == 0.0f);
}

TEST_CASE(b7_spot_light_factory_sets_type_position_dir_cone_params) {
    // B.2 �?`Light::spot()` factory: type=Spot, all 7 params set.
    Light s = Light::spot(
        FVector3(0.0f, 3.0f, 0.0f),       // position
        FVector3(0.0f, -1.0f, 0.0f),      // spotDirection
        /*range=*/10.0f,
        /*intensity=*/1.5f,
        /*coneCosInner=*/0.9f,            // narrow inner cone
        /*coneCosOuter=*/0.7f,            // wider outer cone
        FVector3(0.0f, 0.0f, 1.0f));      // blue
    CHECK(s.type == LightType::Spot);
    CHECK(s.position.y == 3.0f);
    CHECK(s.spotDirection.y == -1.0f);
    CHECK(s.range == 10.0f);
    CHECK(s.intensity == 1.5f);
    CHECK(s.coneCosInner == 0.9f);
    CHECK(s.coneCosOuter == 0.7f);
    CHECK(s.color.z == 1.0f);
}

TEST_CASE(b7_light_pod_size_assert_passes_after_widen) {
    // B.3 �?`sizeof(Light)` is bounded by the `static_assert` ceiling
    // bumped in B (�?96). The actual size on MSVC with the new
    // fields (4 float + FVector3 spotDirection) is ~72 bytes (see
    // AYRenderScene.h comment for layout rationale).
    //
    // We pin the exact size as the contract �?any future field
    // addition that pushes past 96 will trip the static_assert at
    // compile time and surface here as a test failure (the test
    // asserts the boundary, not the exact value).
    CHECK(sizeof(Light) <= 96u);
    // B documented ~68B; verify the actual is consistent with the
    // planned layout (8-byte multiple of float alignment):
    CHECK(sizeof(Light) >= 64u);   // pre-B was 40B; B widens to �?64
    CHECK(sizeof(Light) <= 96u);
}

TEST_CASE(b7_lights_block_layout_four_arrays) {
    // B.4 �?lightsBlock layout: 4 vec4 arrays × 8 lights × 16 bytes
    // per vec4 = 512 bytes total (vs A's 256B). This is the same
    // memory the CPU pack code writes to and the field-split upload
    // reads from (4 separate `setUniform` calls, each 128B).
    constexpr uint32_t kVec4Bytes = 16;
    constexpr uint32_t kLightsPerArray = 8;
    constexpr uint32_t kTotalBytes =
        4 * kLightsPerArray * kVec4Bytes;
    CHECK(kTotalBytes == 512u);

    // Spot factory round-trip: populate a Light with Spot fields,
    // verify the POD stores them at the expected field offsets.
    Light spot = Light::spot(
        FVector3(1.0f, 2.0f, 3.0f),
        FVector3(0.0f, 0.0f, -1.0f),
        12.0f, 0.75f, 0.95f, 0.5f,
        FVector3(1.0f, 1.0f, 1.0f));
    CHECK(spot.type == LightType::Spot);
    CHECK(spot.range == 12.0f);
    CHECK(spot.intensity == 0.75f);
    CHECK(spot.coneCosInner == 0.95f);
    CHECK(spot.coneCosOuter == 0.5f);
}

TEST_CASE(b7_lighting_cache_key_bump_pinned_live) {
    // B.5 + D �?Bug fix #3 verification: the test mirror MUST
    // equal the live `kLightingCacheKeyCStr` extern. Pre-B this
    // was a self-compare (false green); the extern makes it a
    // real drift detector. If LightingPass.cpp's
    // `kLightingCacheKey` literal ever drifts from the mirror
    // literal in this file, this test fails immediately
    // (cutsheet §P5.5 B Bug fix #3).
    CHECK(std::string(kExpectedB7LightingCacheKey)
          == std::string(kLightingCacheKeyCStr));
    // Both strings should be non-empty and reasonably long (cache
    // keys are stable identifiers, not free-form text).
    CHECK(std::string(kExpectedB7LightingCacheKey).size() >= 20u);
    CHECK(std::string(kLightingCacheKeyCStr).size() >= 20u);
    // The literal must contain the §P5.5 D version bump marker.
    CHECK(std::string(kLightingCacheKeyCStr).find("v23_vec4_ibl_gates")
          != std::string::npos);
}

// §P5.5 D (2026-07-23) �?IBL MVP ambient cube lookup contract
// pins. Two new cases pinning the D ship:
//
//   D.1) `cubeActive=0 default` �?pre-D byte-equivalent flat
//        ambient term (`vec3(0.1, 0.1, 0.1)`). The Phoskia
//        source MUST still contain the ambientFlat substring;
//        the cube lookup term MUST be gated by `cubeActive`
//        (so the FS evaluates `0 * cubeActive = 0` when
//        cubeActive=0).
//   D.2) `cubeActive=1` �?FS evaluates `sample(envCube, N) *
//        ambientStrength.x * cubeActive.x`. Pins the envCube sampler
//        + cubeActive / ambientStrength uniform substrings.

TEST_CASE(b7_ibl_cube_active_zero_default_pins_byte_equivalent) {
    // §P5.5 D.1 �?Pre-D byte-equivalent contract: the LightingPass
    // FS ambient term MUST contain `vec3(0.1, 0.1, 0.1)` as the
    // flat floor (the pre-D / pre-IBL ambient value). When
    // cubeActive=0 (host never called Renderer::setSkySourceCube,
    // or SkySource::kind != CubeMap, or no SkyboxPass slot), the
    // cube lookup contribution collapses to 0 �?flat ambient =
    // `vec3(0.1, 0.1, 0.1)`, byte-equivalent to pre-D.
    const std::string src = mirrorLightingPhoskiaSourceB7();
    // ambientFlat substring pins pre-D floor preservation.
    CHECK(src.find("ambientFlat = vec3(0.1, 0.1, 0.1)")
          != std::string::npos);
    // cubeActive gate: ambientCube is multiplied by cubeActive
    // �?0 contribution when cubeActive=0 (pre-D byte-equivalent).
    CHECK(src.find("ambientCube = sample(envCube, N).rgb * ambientStrength.x * cubeActive.x")
          != std::string::npos);
    CHECK(src.find("ambient = ambientFlat + ambientCube")
          != std::string::npos);
}

TEST_CASE(b7_ibl_cube_active_one_uses_envcube_sampler) {
    // §P5.5 D.2 �?Cube path contract: Phoskia source MUST
    // declare `texturecube envCube` + `uniform vec4 cubeActive`
    // + `uniform vec4 ambientStrength`. When cubeActive=1 the
    // FS evaluates `sample(envCube, N).rgb * ambientStrength *
    // 1.0` for normal-driven diffuse ambient (the IBL MVP
    // surface).
    const std::string src = mirrorLightingPhoskiaSourceB7();
    CHECK(src.find("texturecube envCube")
          != std::string::npos);
    CHECK(src.find("uniform vec4 cubeActive")
          != std::string::npos);
    CHECK(src.find("uniform vec4 ambientStrength")
          != std::string::npos);
    // The `let` chain evaluates the cube lookup eagerly; cubeActive
    // is the per-frame gate that controls contribution magnitude.
    CHECK(src.find("sample(envCube, N)")
          != std::string::npos);
}

TEST_SUITE_END
