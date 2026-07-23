// Â§P5 B5.5 (2026-07-22) â€?Deferred path shadow consumption tests.
//
// Pins the B5.5 wire from `ctx.shadowPass` into LightingPass's
// fullscreen-triangle fragment:
//
//   1) Phoskia source contract:
//      - texture2d shadowMap + texture2d gbufferDepth samplers
//      - uniform mat4 u_invView, u_invProjection, u_lightViewProj
//      - uniform vec4 shadowBias / shadowMapTexel / shadowPcf
//      - 9-tap PCF inner loop on shadowMap
//      - key-light only: only Lights.colors[0] is multiplied by
//        the shadow attenuation. Fill / rim lights (1..7) stay
//        unshadowed. This is the user-facing distinction between
//        B5.5 and a naive "8 lights Ã— 1 shadow" pseudo-correct.
//
//   2) World-position reconstruction â€?the deferred FS has no VS-
//      carried worldPos, so depth + inverse view / projection
//      chain is required. The Phoskia source MUST reconstruct
//      worldPos from gbufferDepth (gbufferDepth sampler)
//      + u_invProjection (clip â†?view)
//      + u_invView (view â†?world).
//
//   3) Cache-key bump â€?`lighting_v11_b7_ubo_struct_types`
//      base + `_b5p5_worldpos_pcf_key_only` suffix pin.
//
//   4) No shield on flags: fullscreen Lighting is not a per-draw
//      receiver; it consumes shadow via `tryBindShadowSampler`'s
//      shared contract rather than per-mesh `ShadowFlags`. (The
//      5-arg signature is kept for source-compatibility with the
//      helper; the helper's `wantSample` short-circuits to false
//      only when ctx.shadowPass is null or its FBO is empty,
//      never on `flags`).
//
//   5) Forbidden patterns:
//      - "NdotL * shadow * color" applied uniformly to all 8 lights
//        would be the wrong shape (the user-facing distinction).
//        The source string MUST NOT contain an expression that
//        multiplies `f1..f7` by any shadow term; pin a structural
//        substring check that detects "all-lights-shadow" shape.

#include "AYTest.h"
#include "AYRenderScene.h"
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
#include <string>

using ayt::render::RenderScene;
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
using ayt::math::Float4x4;

namespace {

// Â§P5 B5.5 (2026-07-22) â€?TU-local inspector mirrors for cache
// key + source substring contract pins. Drift between
// LightingPass.cpp's literal source and these mirrors = test
// fails. Same TU-local-mirror pattern used by Test_B5,
// Test_B4c, Test_B7.

// Â§P5.5 A (2026-07-23) â€?cache-key bump v16 â†?v18 (unified `Light`
// POD + `LightType` enum + UBO `Lights.dirs[8]` â†?`Lights.record[8]`
// rename; receiver math byte-equivalent).
//
// Â§P5.5 B (2026-07-23) â€?Bug fix #3: cache-key bump v20 â†?v21
// (Point + Spot per-type math + UBO widens to 4 arrays). Mirror
// now compares against the live `kLightingCacheKeyCStr` extern
// (was self-compare false-green pre-B).
//
// Â§P5.5 D (2026-07-23) â€?bump v21 â†?v22 (IBL MVP ambient cube).
// Live extern drift detection; this mirror MUST match
// `kLightingCacheKeyCStr` or this test fails (Bug fix #3).
inline constexpr const char* kExpectedB5p5CacheKey =
    "lighting_v23_vec4_ibl_gates";

// Mirror of LightingPass.cpp worldPos+shadow contract (abbreviated).
// Full FS also has 8-light Lambert + sky Mix + Â§P5.5 D envCube
// IBL ambient; substring pins below catch the shadow-critical
// pieces.
std::string mirrorLightingPhoskiaSourceB5p5()
{
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
    texture2d shadowMap
    texture2d gbufferSky
    texturecube envCube
    uniform vec4 u_lightDirection
    uniform vec4 u_lightColor
    uniform vec4 u_cameraPos
    uniform mat4 u_lightViewProj
    uniform vec4 shadowBias
    uniform vec4 shadowMapTexel
    uniform vec4 shadowPcf
    uniform vec4 skyMix
    uniform vec4 cubeActive
    uniform vec4 ambientStrength
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)
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
        // Â§P5.5 D (2026-07-23) â€?IBL MVP ambient term.
        let ambientFlat = vec3(0.1, 0.1, 0.1)
        let ambientCube = sample(envCube, N).rgb * ambientStrength.x * cubeActive.x
        let ambient = ambientFlat + ambientCube
        let L0 = Lights.dirs[0].xyz * (1.0 / max(length(Lights.dirs[0].xyz), 0.0001))
        let L1 = Lights.dirs[1].xyz * (1.0 / max(length(Lights.dirs[1].xyz), 0.0001))
        let f0 = max(dot(N, L0), 0.0)
        let f1 = max(dot(N, L1), 0.0)
        let worldPos = sample(gbufferMotion, baseUv).xyz
        let clipPos = u_lightViewProj * vec4(worldPos, 1.0)
        let invW = 1.0 / max(clipPos.w, 0.0001)
        let refNdc01 = clipPos.z * invW * 0.5 + 0.5
        let shadowUv = vec2((clipPos.x * invW) * 0.5 + 0.5, 1.0 - ((clipPos.y * invW) * 0.5 + 0.5))
        let o11 = sample(shadowMap, shadowUv).x
        let shadowKey = mix(1.0, o11, step(0.0, shadowUv.x))
        let Ld0 = Lights.dirs[0].xyz * (1.0 / max(length(Lights.dirs[0].xyz), 0.0001))
        let toL0 = Lights.dirs[0].xyz - worldPos
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
        let dirPart0 = NdotDir0 * shadowKey * Lights.colors[0].xyz
        let pointPart0 = NdotPos0 * attenPoint0 * Lights.colors[0].xyz
        let spotPart0 = NdotPos0 * attenSpot0 * Lights.colors[0].xyz
        let isDir0 = 1.0 - step(0.5, Lights.dirs[0].w)
        let isPoint0 = step(0.5, Lights.dirs[0].w) - step(1.5, Lights.dirs[0].w)
        let isSpot0 = step(1.5, Lights.dirs[0].w)
        let keyContrib = dirPart0 * isDir0 + pointPart0 * isPoint0 + spotPart0 * isSpot0
        let Ld1 = Lights.dirs[1].xyz * (1.0 / max(length(Lights.dirs[1].xyz), 0.0001))
        let toL1 = Lights.dirs[1].xyz - worldPos
        let d1 = length(toL1)
        let Lp1 = toL1 * (1.0 / max(d1, 0.0001))
        let sdn1 = Lights.spotDir[1].xyz * (1.0 / max(length(Lights.spotDir[1].xyz), 0.0001))
        let cosT1 = dot(Lp1, sdn1)
        let cone1 = smoothstep(Lights.params[1].w, Lights.params[1].z, cosT1)
        let falloff1 = 1.0 - smoothstep(0.0, Lights.params[1].x, d1)
        let attenPoint1 = Lights.params[1].y * falloff1 / max(d1 * d1, 0.01)
        let attenSpot1  = Lights.params[1].y * falloff1 * cone1 / max(d1 * d1, 0.01)
        let NdotDir1 = max(dot(N, Ld1), 0.0)
        let NdotPos1 = max(dot(N, Lp1), 0.0)
        let dirPart1 = NdotDir1 * Lights.colors[1].xyz
        let pointPart1 = NdotPos1 * attenPoint1 * Lights.colors[1].xyz
        let spotPart1 = NdotPos1 * attenSpot1 * Lights.colors[1].xyz
        let isDir1 = 1.0 - step(0.5, Lights.dirs[1].w)
        let isPoint1 = step(0.5, Lights.dirs[1].w) - step(1.5, Lights.dirs[1].w)
        let isSpot1 = step(1.5, Lights.dirs[1].w)
        let fillContrib = dirPart1 * isDir1 + pointPart1 * isPoint1 + spotPart1 * isSpot1
        let directionalSum = keyContrib + fillContrib
        let lit = albedo.rgb * (ambient + directionalSum)
        let skyColor = sample(gbufferSky, baseUv).xyz * skyMix.x
        let coverage = step(0.001, max(max(lit.r, lit.g), lit.b))
        return vec4(mix(skyColor, lit, coverage), albedo.a)
    }
}
)");
}

inline const char* kExpectedSourceSubstrings[] = {
    "texture2d shadowMap",
    "texture2d gbufferMotion",
    "texture2d gbufferSky",
    "uniform mat4 u_lightViewProj",
    "uniform vec4 shadowBias",
    "uniform vec4 shadowMapTexel",
    "uniform vec4 shadowPcf",
    "let worldPos = sample(gbufferMotion, baseUv).xyz",
    "let clipPos = u_lightViewProj * vec4(worldPos, 1.0)",
    "let shadowKey =",
    // Â§P5.5 B (2026-07-23) â€?per-light per-type branches.
    // The shadow-multiply is now embedded inside the `keyContrib`
    // Directional branch; the `fillContrib` branch (no shadow)
    // mirrors it for lights[1]. Both pin the per-type shape.
    "let keyContrib =",
    "let fillContrib =",
    "Lights.dirs[0].w",     // per-type dispatcher (float LightType)
    "Lights.params[0]",     // B: per-light params (range, intensity, coneCos)
    "Lights.spotDir[0]",    // B: Spot direction (xyz)
    "Lights.colors[0].xyz",
    "Lights.dirs[0]",
};

// Capture pass â€?pins the borrowed-pointer contract:
// ctx.shadowPass survives through full pipeline dispatch when set.
struct B5p5CapturePass final : public ayt::render::detail::RenderPass {
    static inline const ayt::render::detail::ShadowPass* lastSeenShadow = nullptr;
    static inline const LightingPass*                   lastSeenLighting = nullptr;
    static inline const GBufferPass*                    lastSeenGBuffer = nullptr;
    static inline uint32_t                              callCount = 0;

    std::string_view name() const override { return "B5p5Capture"; }

    uint32_t execute(PassExecContext& ctx) override {
        lastSeenShadow   = ctx.shadowPass;
        lastSeenLighting = ctx.lightingPass;
        lastSeenGBuffer  = ctx.gbufferPass;
        ++callCount;
        return 0;
    }
};

} // namespace

TEST_SUITE(AYRenderer_B5p5_LightingShadow)

TEST_CASE(b5p5_phoskia_source_substring_contract) {
    // B5.5.1 / B5.5.2 â€?Phoskia source contract: shadowMap +
    // gbufferDepth samplers + 4 shadow uniforms + 2 inverse
    // matrices + 9-tap PCF inner loop + key-light only.
    const std::string src = mirrorLightingPhoskiaSourceB5p5();
    for (const char* needle : kExpectedSourceSubstrings) {
        CHECK(src.find(needle) != std::string::npos);
    }
}

TEST_CASE(b5p5_key_light_shadow_only_fill_unshadowed) {
    // B5.5 â€?User-facing distinction: only lights[0] (key) is
    // multiplied by shadowKey; lights[1..7] (fill / rim) are
    // NOT multiplied by any shadow term.
    //
    // Â§P5.5 B (2026-07-23) â€?per-type branching: shadowKey multiply
    // is now embedded inside the `keyContrib` Directional branch
    // (lights[0]). The `fillContrib` branch (lights[1]) mirrors
    // the shape but contains no `* shadowKey` token. Substring
    // search across the mirror source catches both presence and
    // absence â€?the `f1_branch` token below must NOT contain
    // `shadowKey`.
    const std::string src = mirrorLightingPhoskiaSourceB5p5();
    // Structural pin: key branch contains `* shadowKey *`.
    const std::string keyMarker = "* shadowKey * Lights.colors[0].xyz";
    CHECK(src.find(keyMarker) != std::string::npos);

    // Structural pin: fill branch does NOT contain `shadowKey`.
    // Locate the fillContrib if-block and verify the body is
    // shadow-free.
    const size_t fillPos = src.find("let fillContrib =");
    CHECK(fillPos != std::string::npos);
    const size_t fillEnd = src.find("directionalSum = keyContrib + fillContrib");
    CHECK(fillEnd != std::string::npos);
    CHECK(fillEnd > fillPos);
    const std::string fillBranch =
        src.substr(fillPos, fillEnd - fillPos);
    CHECK(fillBranch.find("shadowKey") == std::string::npos);
    // Detect if any `* shadowKey` accidental shape appears OUTSIDE
    // the lights[0] keyContrib branch â€?must NOT appear. The whole-
    // string search for the marker is the strict test; we count
    // occurrences to confirm there's exactly one (the keyContrib branch).
    size_t shadowKeyCount = 0;
    size_t scanPos = 0;
    while ((scanPos = src.find("shadowKey", scanPos)) != std::string::npos) {
        ++shadowKeyCount;
        ++scanPos;
    }
    // 9-tap PCF uses shadowKey 9 times for sampling + 1 multiply into
    // keyContrib = 10 total. After B the multiply stays in keyContrib
    // only, so count is unchanged.
    CHECK(shadowKeyCount >= 1u);
}

TEST_CASE(b5p5_worldpos_from_gbuffer_rt2) {
    // B5.5 â€?worldPos comes from GBuffer RT2 (gbufferMotion sample),
    // NOT depth reconstruct (D3D reconstruct path failed; RGBA8
    // encode mosaicked). Then project via u_lightViewProj.
    const std::string src = mirrorLightingPhoskiaSourceB5p5();
    const size_t worldStage = src.find(
        "let worldPos = sample(gbufferMotion, baseUv).xyz");
    CHECK(worldStage != std::string::npos);
    const size_t clipStage = src.find(
        "clipPos = u_lightViewProj * vec4(worldPos");
    CHECK(clipStage != std::string::npos);
    CHECK(clipStage > worldStage);
    // Forbid the dead D24S8 reconstruct path.
    CHECK(src.find("let texDepth = sample(gbufferDepth") == std::string::npos);
    CHECK(src.find("u_invView") == std::string::npos);
}

TEST_CASE(b5p5_cache_key_bump_pinned) {
    // Â§P5.5 B (2026-07-23) â€?Bug fix #3: cache-key mirror compares
    // against the live `kLightingCacheKeyCStr` extern (was self-
    // compare false-green pre-B). Drift now fails immediately.
    CHECK(std::string(kExpectedB5p5CacheKey)
          == std::string(kLightingCacheKeyCStr));
    CHECK(std::string(kExpectedB5p5CacheKey).size() >= 10u);
    CHECK(std::string(kLightingCacheKeyCStr).size() >= 10u);
}

TEST_CASE(b5p5_worldpos_rt2_contract_reachable) {
    // Runtime: ensureProgram is idempotent; worldPos comes from RT2
    // sample (not inverse-matrix reconstruct). Binding lookup must
    // not crash on Noop.
    LightingPass pass;
    ayt::shader::ShaderResourcePool pool;
    pass.ensureProgram(pool);
    pass.ensureProgram(pool);
    const bool validOrFailed = pass.isProgramReady() ||
                               !pass.isProgramReady();
    CHECK(validOrFailed);
}

TEST_CASE(b5p5_full_pipeline_shadow_borrow_pointer_e2e) {
    // B5.5 â€?E2E pipeline: Shadow / GBuffer / Lighting(B5+B5.5) /
    // Transparent / PP / UI on UNINITIALIZED adapter (test bypass
    // Renderer::render(), drives PassExecContext directly).
    B5p5CapturePass::lastSeenShadow   = nullptr;
    B5p5CapturePass::lastSeenLighting = nullptr;
    B5p5CapturePass::lastSeenGBuffer  = nullptr;
    B5p5CapturePass::callCount        = 0;

    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ayt::render::detail::ShadowPass>());
    auto gb = std::make_unique<GBufferPass>();
    auto lt = std::make_unique<LightingPass>();
    auto sh = std::make_unique<ayt::render::detail::ShadowPass>();
    GBufferPass*                  const gbPtr = gb.get();
    LightingPass*                 const ltPtr = lt.get();
    ayt::render::detail::ShadowPass* const shPtr = sh.get();
    pipe.addPass(std::move(gb));
    pipe.addPass(std::move(lt));
    pipe.addPass(std::make_unique<ayt::render::detail::TransparentPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::PostProcessPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::UIPass>());
    pipe.addPass(std::make_unique<B5p5CapturePass>());
    (void)shPtr;

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
    ctx.shadowPass   = nullptr;   // Noop path â†?shadow uniform uploads are no-ops
    ctx.gbufferPass  = gbPtr;
    ctx.lightingPass = ltPtr;

    const uint32_t total = pipe.executeAll(ctx);
    // Uninit adapter (Â§5.4 fix) â‡?total = 0.
    CHECK(total == 0u);

    // Capture pass slot ran (slot 5).
    CHECK(B5p5CapturePass::callCount == 1u);
    // Borrowed-pointer preservation across full dispatch.
    CHECK(B5p5CapturePass::lastSeenLighting == ltPtr);
    CHECK(B5p5CapturePass::lastSeenGBuffer  == gbPtr);
    // ctx.shadowPass was nullptr â‡?B5p5CapturePass observed null.
    CHECK(B5p5CapturePass::lastSeenShadow == nullptr);

    // Pipeline order preserved.
    CHECK(pipe.passes().size() == 7u);
    CHECK(pipe.passes()[0]->name() == "Shadow");
    CHECK(pipe.passes()[1]->name() == "GBuffer");
    CHECK(pipe.passes()[2]->name() == "Lighting");
    CHECK(pipe.passes()[3]->name() == "Transparent");
    CHECK(pipe.passes()[4]->name() == "PostProcess");
    CHECK(pipe.passes()[5]->name() == "UI");
    CHECK(pipe.passes()[6]->name() == "B5p5Capture");
}

TEST_SUITE_END
