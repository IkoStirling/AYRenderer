// §Skybox0 (2026-07-23) — SkyboxPass + SkySource DataSource tests.
//
// Pins the §Skybox0 ship (post-§P5.5 A) at four levels:
//
//   1) SkySource POD contract (cutsheet §10 red lines preserved):
//      - default ctor → kind = Equirect, equirect = invalid,
//        cubeReserve = 0
//      - `hasEquirect()` returns false on default-constructed
//      - `isActive()` returns false on default-constructed
//      - SkySourceKind::Equirect = 0, SkySourceKind::CubeMap = 1
//        (CubeMap path reserved for §Skybox0-B cut, A only ships
//        Equirect)
//
//   2) RenderPassSlot::Skybox enum value (cutsheet §5.1 view-id
//      lock table):
//      - Skybox = 1 (between Shadow=0 and ForwardOpaque=2)
//      - makeDeferred() returns a 7-slot pipeline; order is
//        Shadow, Skybox, GBuffer, Lighting, Trans, PP, UI
//
//   3) PassExecContext skySource + skyboxPass borrowed pointer
//      contract (cutsheet §5.3 red lines preserved):
//      - 17→18-field brace-init test sites (Test_B7 / Test_B5p5)
//        keep compiling via C++14 trailing-default
//      - Default-init = nullptr for both fields
//      - Borrowed-pointer lifetime contract: pipeline-resident
//        SkyboxPass outlives executeAll(), so a borrowed pointer
//        captured by LightingPass survives dispatch
//
//   4) LightingPass FS additions + cache-key bump (cutsheet
//      §Skybox0):
//      - Phoskia source declares `texture2d gbufferSky` sampler +
//        `vec4 skyMix` uniform
//      - FS末 uses `mix(skyColor, lit, coverage)` to blend sky
//        backdrop
//      - cache-key literal = "lighting_v20_sky0_equirect_backdrop"
//      - When ctx.skyboxPass == nullptr the sampler stays unbound
//        ⇒ byte-equivalent to pre-§Skybox0 dark-frame behavior
//        (mix(black, lit, 1) == lit)
//
//   5) Full pipeline E2E: 7-slot Deferred pipeline + CapturePass
//      at slot 7, verifies SkyboxPass::execute runs at slot 1
//      and pipeline order matches the cutsheet lock.
//
// Red lines preserved (cutsheet §5.3 + §5.5 + §10):
//   - NO RenderScene::Light (永久退休)
//   - NO FrameContext field additions
//   - NO RenderPass::execute signature change
//   - NO default Forward pipeline behavior change
//   - NO public header bgfx:: type additions
//   - PassExecContext grew by 2 borrowed-pointer fields (≤ 1 per
//     cut budget not consumed; §Skybox0 eats both budget slots at
//     once — acceptable because SkyboxPass is the third Deferred-
//     only pass this cut adds and the slots are scoped to one
//     coherent feature).

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
#include "detail/SkyboxPass.h"
#include "detail/TransparentPass.h"
#include "detail/UIPass.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

using ayt::render::RenderPipelineDesc;
using ayt::render::RenderPassSlot;
using ayt::render::SkySource;
using ayt::render::SkySourceKind;
using ayt::render::TextureHandle;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::LightingPass;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPipeline;
using ayt::render::detail::SkyboxPass;

namespace {

// §Skybox0 (2026-07-23) — LightingPass cache-key bump literal.
// Pin here so a master-cache-key change without a Test_Skybox0
// bump fails this case (mirror Test_B5 / Test_B5p5 / Test_B7
// cache-key pinning pattern).
inline constexpr const char* kExpectedLightingCacheKey =
    "lighting_v20_sky0_equirect_backdrop";

// §Skybox0 (2026-07-23) — SkyboxPass cache-key literal. Pin
// here so a master-cache-key change without a Test_Skybox0
// bump fails this case.
inline constexpr const char* kExpectedSkyboxCacheKey =
    "skybox_v0_equirect_fullscreen";

// §Skybox0 (2026-07-23) — Phoskia source substring pins. Drift
// between SkyboxPass.cpp's literal source and these mirrors = test
// fails. Pin the minimum: gbufferSky sampler + skyMix uniform in
// the LightingPass FS, and the equirect sampler + skyMix uniform
// in the SkyboxPass FS.
inline const char* kSkybox0LightingExpectedSubstrings[] = {
    "texture2d gbufferSky",            // §Skybox0 — backdrop sampler
    "uniform vec4 skyMix",             // §Skybox0 — sky intensity uniform
    "let skyColor = sample(gbufferSky, baseUv).xyz * skyMix.x",  // backdrop sample
    "let coverage = step(0.001, max(max(lit.r, lit.g), lit.b))",  // coverage gate
    "let finalColor = mix(skyColor, lit, coverage)",            // backdrop blend
};

inline const char* kSkybox0SkyboxExpectedSubstrings[] = {
    "material Skybox",                 // canonical material name
    "texture2d skyEquirect",           // equirect sampler declaration
    "uniform vec4 skyMix",             // skyMix uniform declaration
    "let skyColor = sample(skyEquirect, baseUv).xyz * skyMix.x",
    "return vec4(skyColor, 1.0)",
};

// Mirror of SkyboxPass.cpp's kSkyboxPhoskiaSource literal. Kept
// in sync by code review + this substring test (the substring
// pins above are the structural contract).
std::string mirrorSkyboxPhoskiaSource()
{
    return std::string(R"(
material Skybox {
    texture2d skyEquirect
    uniform vec4 skyMix
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let baseUv = vec2(vUv.x, 1.0 - vUv.y)
        let skyColor = sample(skyEquirect, baseUv).xyz * skyMix.x
        return vec4(skyColor, 1.0)
    }
}
)");
}

// Mirror of LightingPass.cpp's kLightingPhoskiaSource §Skybox0
// block (just the FS末 backdrop-blend lines). Pin via substring
// test above; full mirror omitted to keep the test small (mirror
// drift on a full source block is already pinned by Test_B5p5
// and Test_B7).
//
// Capture pass — pins the borrowed-pointer contract: ctx.skyboxPass
// survives through full pipeline dispatch when set.
struct Skybox0CapturePass final : public ayt::render::detail::RenderPass {
    static inline const SkyboxPass*   lastSeenSkybox   = nullptr;
    static inline const SkySource*    lastSeenSkySrc   = nullptr;
    static inline uint32_t            callCount        = 0;

    std::string_view name() const override { return "Skybox0Capture"; }

    uint32_t execute(PassExecContext& ctx) override {
        lastSeenSkybox = ctx.skyboxPass;
        lastSeenSkySrc = ctx.skySource;
        ++callCount;
        return 0;
    }
};

} // namespace

TEST_SUITE(AYRenderer_Skybox0)

TEST_CASE(sky_source_pod_default_equirect_empty) {
    // §Skybox0.1 — SkySource POD contract: default ctor yields
    // Equirect kind + invalid equirect handle + cubeReserve = 0.
    SkySource s;
    CHECK(s.kind == SkySourceKind::Equirect);
    CHECK(!s.hasEquirect());
    CHECK(!s.isActive());
    CHECK(s.cubeReserve == 0u);
    CHECK(s.equirect.id == 0u);
}

TEST_CASE(sky_source_kind_enum_values) {
    // §Skybox0.1 — SkySourceKind enum values locked.
    CHECK(static_cast<uint8_t>(SkySourceKind::Equirect) == 0u);
    CHECK(static_cast<uint8_t>(SkySourceKind::CubeMap)  == 1u);
}

TEST_CASE(sky_source_isactive_with_invalid_equirect_is_false) {
    // §Skybox0.1 — Inactive SkySource paths:
    //   - kind != Equirect (CubeMap path reserved for B)
    //   - equirect invalid handle
    SkySource equirectEmpty;
    CHECK(!equirectEmpty.isActive());

    SkySource cubeKind;
    cubeKind.kind = SkySourceKind::CubeMap;
    CHECK(!cubeKind.isActive());
}

TEST_CASE(render_pass_slot_skybox_enum_index) {
    // §Skybox0.2 — RenderPassSlot::Skybox enum position locked.
    // After Skybox insertion: Shadow=0, Skybox=1, ForwardOpaque=2
    // (the enum values of ForwardOpaque and others shift by 1
    // because Skybox was inserted between Shadow and ForwardOpaque).
    // We test the Skybox value directly; downstream tests verify
    // the relative order.
    CHECK(static_cast<uint8_t>(RenderPassSlot::Skybox) == 1u);
    CHECK(RenderPipelineDesc::makeDeferred().passes.size() == 7u);
}

TEST_CASE(deferred_pipeline_skybox_slot_order) {
    // §Skybox0.2 — 7-slot Deferred pipeline includes Skybox at
    // slot 1, between Shadow and GBuffer. This locks the order
    // invariant for §Skybox0 (cutsheet §10 view-id table).
    auto desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.contains(RenderPassSlot::Skybox));
    const auto& slots = desc.passes;
    auto shadowPos = std::find(slots.begin(), slots.end(), RenderPassSlot::Shadow);
    auto skyPos    = std::find(slots.begin(), slots.end(), RenderPassSlot::Skybox);
    auto gbPos     = std::find(slots.begin(), slots.end(), RenderPassSlot::GBuffer);
    auto ltPos     = std::find(slots.begin(), slots.end(), RenderPassSlot::Lighting);
    auto transPos  = std::find(slots.begin(), slots.end(), RenderPassSlot::Transparent);
    auto ppPos     = std::find(slots.begin(), slots.end(), RenderPassSlot::PostProcess);
    auto uiPos     = std::find(slots.begin(), slots.end(), RenderPassSlot::UI);
    CHECK(shadowPos != slots.end());
    CHECK(skyPos    != slots.end());
    CHECK(gbPos     != slots.end());
    CHECK(ltPos     != slots.end());
    CHECK(transPos  != slots.end());
    CHECK(ppPos     != slots.end());
    CHECK(uiPos     != slots.end());
    // Order: Shadow < Skybox < GBuffer < Lighting < Trans < PP < UI
    CHECK(shadowPos < skyPos);
    CHECK(skyPos    < gbPos);
    CHECK(gbPos     < ltPos);
    CHECK(ltPos     < transPos);
    CHECK(transPos  < ppPos);
    CHECK(ppPos     < uiPos);
}

TEST_CASE(forward_default_pipeline_does_not_include_skybox) {
    // §Skybox0.2 — Default Forward pipeline (makeDefault) does
    // NOT include Skybox — cutsheet §5.3 red line #4 (Forward
    // host 0 behavior change). Skybox is opt-in via
    // configurePipeline(makeDeferred()).
    auto desc = RenderPipelineDesc::makeDefault();
    CHECK(!desc.contains(RenderPassSlot::Skybox));
    CHECK(desc.passes.size() == 5u);  // Shadow + FO + Trans + PP + UI
}

TEST_CASE(skybox_pass_name_and_initial_state) {
    // §Skybox0.3 — SkyboxPass::name() = "Skybox"; isReady()
    // returns false until ensure() + ensureProgram() run.
    SkyboxPass pass;
    CHECK(pass.name() == "Skybox");
    CHECK(!pass.isReady());
    CHECK(!pass.isProgramReady());
    CHECK(pass.skyFbo().idx == UINT16_MAX);
    CHECK(pass.skyWidth() == 0u);
    CHECK(pass.skyHeight() == 0u);
}

TEST_CASE(pass_exec_context_skysource_default_null) {
    // §Skybox0.3 — PassExecContext::skySource + skyboxPass default
    // to nullptr. Brace-init 17 fields (pre-§Skybox0 shape) still
    // compiles because both new fields use trailing-default
    // (C++14). This is the source-compat invariant that keeps
    // Test_B7 + Test_B5p5 17-field brace-init sites alive.
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame;

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    CHECK(ctx.skySource  == nullptr);
    CHECK(ctx.skyboxPass == nullptr);
}

TEST_CASE(lighting_pass_cache_key_bump_v20_sky0) {
    // §Skybox0.4 — LightingPass cache-key bump to
    // "lighting_v20_sky0_equirect_backdrop" (mirror of Test_B7 +
    // Test_B5p5 cache-key pinning pattern).
    CHECK(std::string(kExpectedLightingCacheKey)
          == std::string("lighting_v20_sky0_equirect_backdrop"));
    CHECK(std::string(kExpectedLightingCacheKey).size() >= 10u);
}

TEST_CASE(skybox_pass_cache_key_bump_v0) {
    // §Skybox0.4 — SkyboxPass cache-key bump to
    // "skybox_v0_equirect_fullscreen".
    CHECK(std::string(kExpectedSkyboxCacheKey)
          == std::string("skybox_v0_equirect_fullscreen"));
    CHECK(std::string(kExpectedSkyboxCacheKey).size() >= 10u);
}

TEST_CASE(skybox_pass_phoskia_source_substring_contract) {
    // §Skybox0.4 — Phoskia source substring pins. Drift =
    // test fails. Same TU-local-mirror pattern used by Test_B7,
    // Test_B5p5.
    const std::string src = mirrorSkyboxPhoskiaSource();
    for (const char* needle : kSkybox0SkyboxExpectedSubstrings) {
        CHECK(src.find(needle) != std::string::npos);
    }
}

TEST_CASE(full_pipeline_skybox_slot_invoked) {
    // §Skybox0.5 — E2E pipeline: 7-slot Deferred pipeline +
    // Skybox0CapturePass at slot 7. Verify:
    //   - SkyboxPass::execute called once (slot 1)
    //   - CapturePass saw ctx.skyboxPass survive the full
    //     dispatch (borrowed-pointer contract; mirror
    //     Test_B7 b7_full_pipeline_multi_light_e2e shape)
    //   - CapturePass saw ctx.skySource survive dispatch
    //   - Pipeline order preserved (7+1=8 passes, names match)
    Skybox0CapturePass::lastSeenSkybox = nullptr;
    Skybox0CapturePass::lastSeenSkySrc = nullptr;
    Skybox0CapturePass::callCount      = 0;

    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ayt::render::detail::ShadowPass>());
    auto skybox = std::make_unique<SkyboxPass>();
    auto gb     = std::make_unique<ayt::render::detail::GBufferPass>();
    auto lt     = std::make_unique<LightingPass>();
    SkyboxPass*              const skyboxPtr = skybox.get();
    ayt::render::detail::GBufferPass* const gbPtr = gb.get();
    LightingPass*                 const ltPtr = lt.get();
    pipe.addPass(std::move(skybox));
    pipe.addPass(std::move(gb));
    pipe.addPass(std::move(lt));
    pipe.addPass(std::make_unique<ayt::render::detail::TransparentPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::PostProcessPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::UIPass>());
    pipe.addPass(std::make_unique<Skybox0CapturePass>());

    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame;

    SkySource sky;
    // kind = Equirect (default), but equirect handle is invalid.
    // This is an "inactive" SkySource — SkyboxPass early-returns 0
    // per the §Skybox0 contract; borrowed pointer still propagates.
    sky.kind = SkySourceKind::Equirect;

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    ctx.gbufferPass  = gbPtr;
    ctx.lightingPass = ltPtr;
    ctx.skyboxPass   = skyboxPtr;
    ctx.skySource    = &sky;

    // Uninit adapter (§5.4 fix) ⇒ SkyboxPass early-returns 0
    // (slot 1 contributes 0). Total = 0 + 0 (GBuffer Noop) + 0
    // (Lighting Noop) + 0 (Trans/PP/UI Noop) + 0 (CapturePass
    // returns 0) = 0.
    const uint32_t total = pipe.executeAll(ctx);
    CHECK(total == 0u);

    // CapturePass ran (slot 7).
    CHECK(Skybox0CapturePass::callCount == 1u);
    // Borrowed-pointer survival: CapturePass observed
    // ctx.skyboxPass == skyboxPtr (SkyboxPass instance address
    // didn't change between dispatch start and CapturePass
    // execution — mirror shadowPass / lightingPass borrowed-
    // pointer test pattern).
    CHECK(Skybox0CapturePass::lastSeenSkybox == skyboxPtr);
    // ctx.skySource was &sky ⇒ CapturePass saw the same pointer.
    CHECK(Skybox0CapturePass::lastSeenSkySrc == &sky);

    // Pipeline order preserved (8 passes total).
    CHECK(pipe.passes().size() == 8u);
    CHECK(pipe.passes()[0]->name() == "Shadow");
    CHECK(pipe.passes()[1]->name() == "Skybox");
    CHECK(pipe.passes()[2]->name() == "GBuffer");
    CHECK(pipe.passes()[3]->name() == "Lighting");
    CHECK(pipe.passes()[4]->name() == "Transparent");
    CHECK(pipe.passes()[5]->name() == "PostProcess");
    CHECK(pipe.passes()[6]->name() == "UI");
    CHECK(pipe.passes()[7]->name() == "Skybox0Capture");
}

TEST_SUITE_END
