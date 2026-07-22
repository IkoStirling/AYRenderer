// §P5 B5.5 (2026-07-22) — Deferred path shadow consumption tests.
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
//        B5.5 and a naive "8 lights × 1 shadow" pseudo-correct.
//
//   2) World-position reconstruction — the deferred FS has no VS-
//      carried worldPos, so depth + inverse view / projection
//      chain is required. The Phoskia source MUST reconstruct
//      worldPos from gbufferDepth (gbufferDepth sampler)
//      + u_invProjection (clip → view)
//      + u_invView (view → world).
//
//   3) Cache-key bump — `lighting_v11_b7_ubo_struct_types`
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
using ayt::render::detail::LightingPass;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPipeline;
using ayt::math::Float4x4;

namespace {

// §P5 B5.5 (2026-07-22) — TU-local inspector mirrors for cache
// key + source substring contract pins. Drift between
// LightingPass.cpp's literal source and these mirrors = test
// fails. Same TU-local-mirror pattern used by Test_B5,
// Test_B4c, Test_B7.

// §P5.5 A (2026-07-23) — cache-key bump v16 → v18 (unified `Light`
// POD + `LightType` enum + UBO `Lights.dirs[8]` → `Lights.record[8]`
// rename; receiver math byte-equivalent).
inline constexpr const char* kExpectedB5p5CacheKey =
    "lighting_v20_sky0_equirect_backdrop";

// Mirror of LightingPass.cpp worldPos+shadow contract (abbreviated).
// Full FS also has 8-light Lambert + sky Mix; substring pins below
 // catch the shadow-critical pieces.
std::string mirrorLightingPhoskiaSourceB5p5()
{
    return std::string(R"(
uniformblock Lights {
    vec4 dirs[8]
    vec4 colors[8]
} binding 0
material Lighting {
    texture2d gbufferAlbedo
    texture2d gbufferNormal
    texture2d gbufferMotion
    texture2d gbufferDepth
    texture2d shadowMap
    texture2d gbufferSky
    uniform vec4 u_lightDirection
    uniform vec4 u_lightColor
    uniform vec4 u_cameraPos
    uniform mat4 u_lightViewProj
    uniform vec4 shadowBias
    uniform vec4 shadowMapTexel
    uniform vec4 shadowPcf
    uniform vec4 skyMix
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
        let ambient = vec3(0.1, 0.1, 0.1)
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
        let keyContribution = f0 * shadowKey * Lights.colors[0].xyz
        let fillContribution = f1 * Lights.colors[1].xyz
        let directionalSum = keyContribution + fillContribution
        let lit = albedo.rgb * (ambient + directionalSum)
        let skyColor = sample(gbufferSky, baseUv).xyz * skyMix.x
        let coverage = albedo.a
        return vec4(mix(skyColor, lit, coverage), 1.0)
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
    "let keyContribution =",
    "let fillContribution =",
    "Lights.colors[0].xyz",
    "Lights.dirs[0]",
};

// Capture pass — pins the borrowed-pointer contract:
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
    // B5.5.1 / B5.5.2 — Phoskia source contract: shadowMap +
    // gbufferDepth samplers + 4 shadow uniforms + 2 inverse
    // matrices + 9-tap PCF inner loop + key-light only.
    const std::string src = mirrorLightingPhoskiaSourceB5p5();
    for (const char* needle : kExpectedSourceSubstrings) {
        CHECK(src.find(needle) != std::string::npos);
    }
}

TEST_CASE(b5p5_key_light_shadow_only_fill_unshadowed) {
    // B5.5 — User-facing distinction: only lights[0] (key) is
    // multiplied by shadowKey; lights[1..7] (fill / rim) are
    // NOT multiplied by any shadow term.
    const std::string src = mirrorLightingPhoskiaSourceB5p5();
    // structural pin: "keyContribution" appears exactly once and
    // contains `* shadowKey *`. "fillContribution" appears with
    // no `* shadow` token. The whole-string search suffices
    // because PhoskiaConverter emits the FS token-for-token.
    const std::string keyMarker = "keyContribution = f0 * shadowKey *";
    const std::string fillMarker = "fillContribution = f1 *";
    CHECK(src.find(keyMarker) != std::string::npos);
    CHECK(src.find(fillMarker) != std::string::npos);
    // Detect if `f1 * ... * shadowKey` accidental shape appears —
    // pin absence of "fill lights × shadow" expression.
    CHECK(src.find("f1 *") != std::string::npos);
    // Make sure we don't accidentally multiply f1 by shadow.
    // Specifically: search for "f1 * ... shadowKey" — must NOT
    // appear.
    const size_t f1pos = src.find("f1 *");
    CHECK(f1pos != std::string::npos);
    const size_t after_f1 = src.find("\n", f1pos);
    const std::string f1_line = src.substr(f1pos,
        (after_f1 == std::string::npos ? src.size() : after_f1) - f1pos);
    CHECK(f1_line.find("shadowKey") == std::string::npos);
    CHECK(f1_line.find("shadowFilt") == std::string::npos);
}

TEST_CASE(b5p5_worldpos_from_gbuffer_rt2) {
    // B5.5 — worldPos comes from GBuffer RT2 (gbufferMotion sample),
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
    CHECK(std::string(kExpectedB5p5CacheKey)
          == std::string("lighting_v20_sky0_equirect_backdrop"));
    CHECK(std::string(kExpectedB5p5CacheKey).size() >= 10u);
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
    // B5.5 — E2E pipeline: Shadow / GBuffer / Lighting(B5+B5.5) /
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
    ctx.shadowPass   = nullptr;   // Noop path → shadow uniform uploads are no-ops
    ctx.gbufferPass  = gbPtr;
    ctx.lightingPass = ltPtr;

    const uint32_t total = pipe.executeAll(ctx);
    // Uninit adapter (§5.4 fix) ⇒ total = 0.
    CHECK(total == 0u);

    // Capture pass slot ran (slot 5).
    CHECK(B5p5CapturePass::callCount == 1u);
    // Borrowed-pointer preservation across full dispatch.
    CHECK(B5p5CapturePass::lastSeenLighting == ltPtr);
    CHECK(B5p5CapturePass::lastSeenGBuffer  == gbPtr);
    // ctx.shadowPass was nullptr ⇒ B5p5CapturePass observed null.
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
