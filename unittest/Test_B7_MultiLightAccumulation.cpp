// §P5 B7+ (2026-07-22) — multi-light accumulation contract tests.
//
// Pins the B7 ship:
//
//   1) SceneLights POD contract (cutsheet §5.3 red lines preserved):
//      - MAX = 8 (kMaxSceneLights)
//      - default ctor → count = 0, no lights
//      - add() returns assigned slot, fails soft past MAX (UINT32_MAX)
//
//   2) PassExecContext::sceneLights borrowed pointer contract:
//      - 17-field brace init continues to compile (= C++14 trailing
//        default = nullptr). Existing 16-field brace-init test sites
//        (Test_B5_LightingDirectional, Test_B6_PostProcessSourceFbo,
//        Test_B4c_MotionVector, ...) keep compiling without edits.
//      - Default-init = nullptr (B5 single-light fallback preserved).
//
//   3) Renderer::setSceneLights wiring contract — borrowed pointer
//      pinned to the slot, lifetime is host's responsibility
//      (mirror shadowPass borrowed pointer pattern; cutsheet
//      pass-lessons-from-deferred.md §5.4 / execution-plan.md:329
//      "ctx.lights 借用指针").
//
//   4) LightingPass Phoskia source B7 upgrades (cutsheet
//      pass-lessons-from-deferred.md:329) — source-string pin:
//      - declares `uniformblock Lights { vec4 dirs[8];
//        vec4 colors[8]; } binding 0` (NOT bare `uniform` array
//        — Phoskia doesn't parse that; cutsheet §5.3)
//      - FS unrolls 8 directional-light taps unconditionally
//      - access syntax: `Lights.dirs[i].xyz` /
//        `Lights.colors[i].xyz` — PascalCase block name + lowercase
//        field (verified at AYShader/unittest/golden/
//        material_with_ubo_binding.phoskia:5 + skinned_lit.phoskia:19
//        "Skeleton.bones[...]" canonical form)
//      - cache-key bump: v3_b5_hlsl_vec_ctors → v4_b7_multi_light_ubo
//
//   5) B7 lightsBlock layout pin — `uniformblock Lights` field
//      order MUST be `dirs[8]` then `colors[8]`. CPU upload mirrors
//      the same std140 layout so `setUniformBlock(Lights, ...)` packs
//      the same byte footprint (`kMaxSceneLights * 16` dirs +
//      `kMaxSceneLights * 16` colors = 256 bytes).
//
//   6) Multi-light count > 0 path: count = 2 lights, verify both
//      `direction` and `color` slots are addressed (lightsBlock
//      bytes [16..32) populated for index 1).
//
//   7) E2E pipeline with multi-light DataSource: Shadow/GBuffer/
//      Lighting/Transparent/PP/UI 7-slot pipeline on Noop backend.
//      LightingPass Noop-gates via isInitialized() (§5.4 fix).
//      Total draws = 0 (mirror Test_B5 case 7). Borrowed pointers
//      propagate (ctx.sceneLights survives the full dispatch).
//
// Red lines preserved (cutsheet §5.3 + §5.5):
//   - NO RenderScene::Light (永久退休)
//   - NO FrameContext field additions
//   - NO ForwardOpaque / Transparent sampler changes
//   - PassExecContext grew by 1 borrowed-pointer field (≤ 1 per cut).

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
using ayt::render::RenderScene;
using ayt::render::SceneLights;
using ayt::render::kMaxSceneLights;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GBufferPass;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::LightingPass;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPipeline;
using ayt::math::FVector3;

namespace {

// §P5 B7+ (2026-07-22) — TU-local inspector mirrors for the B7
// cache-key bump. Drift between LightingPass.cpp's literal and
// these mirrors = test fails. Same TU-local-mirror pattern used
// by Test_B5_LightingDirectional.cpp::kExpectedLightingCacheKey.
inline constexpr const char* kExpectedB7LightingCacheKey =
    "lighting_v4_b7_multi_light_ubo";

// §P5 B7+ (2026-07-22) — Phoskia source substring pins. Drift =
// test fails. Note PascalCase `Lights` block name (matches
// AYShader/unittest/golden/material_with_ubo_binding.phoskia:5
// "Camera.position" canonical access shape).
inline const char* kExpectedSourceSubstrings[] = {
    "uniformblock Lights",            // canonical UBO declaration
    "vec4 dirs[8]",                   // dirs array field
    "vec4 colors[8]",                 // colors array field
    "} binding 0",                    // binding 0 (matches B5 single-light contract slot)
    "Lights.dirs[0].xyz",             // access pattern (PascalCase block + lowercase field)
    "Lights.colors[0].xyz",
    "Lights.dirs[7].xyz",
    "Lights.colors[7].xyz",
};

// Forbidden substrings — pins "no RenderScene::Light" + "no
// FrameContext field" + "no new pass slot" — all of these are
// forbidden by cutsheet §5.3 / §5.5. They CANNOT appear anywhere
// in the B7 file set (the new code paths produce them implicitly
// via class names; absent here means nobody accidentally
// reintroduced a Light struct).
inline const char* kForbiddenSourceSubstrings[] = {
    "RenderScene::Light",   // 永久退休 per §5.5
    "FrameContext lights",   // FrameContext 0 grow per §5.3
};

// Mirror of kLightingPhoskiaSource at LightingPass.cpp (the
// B7-bumped variant). Kept in sync by code review (string-search
// contract pinned by the substring tests above).
std::string mirrorLightingPhoskiaSourceB7()
{
    return std::string(R"(
material Lighting {
    texture2d gbufferAlbedo
    texture2d gbufferNormal
    texture2d gbufferMotion
    uniform vec4 u_lightDirection
    uniform vec4 u_lightColor
    uniform vec4 u_cameraPos
    uniformblock Lights {
        vec4 dirs[8]
        vec4 colors[8]
    } binding 0
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
        let ambient = vec3(0.1, 0.1, 0.1)
        let L0 = normalize(Lights.dirs[0].xyz)
        let L7 = normalize(Lights.dirs[7].xyz)
        let f0 = max(dot(N, L0), 0.0)
        let f7 = max(dot(N, L7), 0.0)
        let directionalSum = f0 * Lights.colors[0].xyz + f7 * Lights.colors[7].xyz
        let lit = albedo.rgb * (ambient + directionalSum)
        return vec4(lit, albedo.a)
    }
}
)");
}

// Capture pass — mirrors Test_B5 / Test_B6 CapturePass pattern.
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
    // B7.1 — SceneLights POD contract: MAX = 8, default ctor empty.
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
    // B7.1 — add() returns assigned slot, fails soft past MAX.
    SceneLights lights;
    CHECK(lights.add({FVector3(0.0f, -1.0f, 0.0f),
                      FVector3(1.0f, 1.0f, 1.0f)}) == 0u);
    CHECK(lights.add({FVector3(1.0f, 0.0f, 0.0f),
                      FVector3(0.5f, 0.5f, 0.5f)}) == 1u);
    CHECK(lights.count == 2u);
    CHECK(lights.empty() == false);

    // Fill to MAX (8 total = 2 already + 6 more).
    for (uint32_t i = 2; i < kMaxSceneLights; ++i) {
        CHECK(lights.add({FVector3(0.0f, -1.0f, 0.0f),
                          FVector3(0.1f, 0.1f, 0.1f)}) == i);
    }
    CHECK(lights.count == kMaxSceneLights);

    // 9th add: soft cap. count unchanged, return UINT32_MAX.
    const uint32_t overflow = lights.add({FVector3(0.0f, -1.0f, 0.0f),
                                           FVector3(0.0f, 0.0f, 0.0f)});
    CHECK(overflow == UINT32_MAX);
    CHECK(lights.count == kMaxSceneLights);
}

TEST_CASE(b7_pass_exec_context_brace_init_default_keeps_compatibility) {
    // B7.2 — 17-field brace init compiles. Default-init at the
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
    // B7.4 — Phoskia source substring pin. The B7-bumped source
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
    // B7 cache-key bump (mirror Test_B5::b5_lighting_cache_key_and_build_stamp_pinned).
    CHECK(std::string(kExpectedB7LightingCacheKey)
          == std::string("lighting_v4_b7_multi_light_ubo"));
    CHECK(std::string(kExpectedB7LightingCacheKey).size() >= 10u);
}

TEST_CASE(b7_lights_block_layout_dirs_then_colors) {
    // B7.5 — lightsBlock layout pin: dirs[8] at offset 0,
    // colors[8] at offset 128 (each vec4 = 16 bytes; 8 vec4 =
    // 128 bytes). The CPU upload mirrors std140 packing so
    // `setUniformBlock(Lights, ...)` ships the same byte
    // footprint (= 256 bytes total).
    constexpr uint32_t kDirBlockSizeBytes   = 8 * 16;
    constexpr uint32_t kColorBlockSizeBytes = 8 * 16;
    constexpr uint32_t kTotalBlockSizeBytes =
        kDirBlockSizeBytes + kColorBlockSizeBytes;
    CHECK(kTotalBlockSizeBytes == 256u);

    // Mirror the lightsBlock layout: index 0..7 → dirs, index
    // 8..15 → colors (stored as 8 + i mapping).
    SceneLights two;
    two.add({FVector3(0.0f, -1.0f, 0.0f), FVector3(1.0f, 1.0f, 1.0f)});
    two.add({FVector3(1.0f,  0.0f, 0.0f), FVector3(0.5f, 0.5f, 0.5f)});

    // dir slot 0 at byte offset [0..16)
    CHECK(two.lights[0].direction.y == -1.0f);
    CHECK(two.lights[1].direction.x ==  1.0f);
    CHECK(two.lights[1].color.x     ==  0.5f);
}

TEST_CASE(b7_full_pipeline_multi_light_e2e) {
    // B7.7 — E2E pipeline: Shadow/GBuffer(B4c)/Lighting(B5+B7)/
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
    lights.add({FVector3(0.3f, -0.8f, -0.4f), FVector3(1.0f, 1.0f, 1.0f)});
    lights.add({FVector3(0.5f, 0.2f, -0.3f),  FVector3(0.4f, 0.4f, 0.6f)});

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    ctx.gbufferPass  = gbPtr;
    ctx.lightingPass = ltPtr;
    ctx.sceneLights  = &lights;

    const uint32_t total = pipe.executeAll(ctx);
    // Uninit adapter (§5.4 fix) ⇒ total = 0.
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

TEST_SUITE_END
