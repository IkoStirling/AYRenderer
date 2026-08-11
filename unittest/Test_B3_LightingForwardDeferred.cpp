// §P5 B3 (2026-07-22) — Forward/Deferred factory + LightingPass
// empty shell smoke tests.
//
// Mirrors Test_B2_GBufferPass.cpp's structure (PR-§P5 B2 plumbing-
// only pass + PassExecContext borrowed pointer). This suite pins:
//
//   1) PassExecContext::lightingPass field exists + default-init
//      nullptr in a 12-field brace-init form. Pins the field is
//      present (a future PR that deletes it would compile-fail this
//      suite + every existing 12-/15-field brace-init test).
//
//   2) LightingPass base contract:
//        - name() == "Lighting"
//        - isReady() == false on construction (no FBO, no program)
//        - static constexpr view-id = 8 (B5 lock per docs/pass-
//          lessons-from-deferred.md §5.1)
//
//   3) execute() Noop-gates cleanly when adapter is uninitialized
//      (returns 0) AND when adapter is initialized but on Noop
//      backend (returns 0). Mirrors ShadowPass::execute + GBuffer-
//      Pass::execute Noop early-exit.
//
//   4) Accessors return BGFX_INVALID_HANDLE / 0 until B5 wires
//      real GPU state. This is the contract downstream consumers
//      (B7+ multi-light, tone-mapping) rely on for "no Lighting
//      output this frame → use fallback path".
//
//   5) destroyResources() is a clean no-op on a shell instance
//      (no crash, no-op cleanup before B5 attaches real FBO +
//      program).
//
//   6) Two-pass pipeline with LightingPass wired + ctx.lightingPass
//      pointer set — full dispatch through executeAll() returns 0
//      on Noop, no crash, no state corruption. Mirrors the B2
//      pointer-handoff test shape.
//
//   7) Forward/Deferred factory-layer integration + public API
//      smoke:
//        - makeDefault() returns 5-slot Forward WITHOUT GBuffer /
//          Lighting (B3 omits Deferred-only slots from Forward)
//        - makeDeferred() returns 6-slot Deferred WITH GBuffer +
//          Lighting AND WITHOUT ForwardOpaque (cutsheet §4.1 red
//          line #4)
//        - Manual {Shadow, GBuffer, Lighting, Trans, PP, UI}
//          pipeline dispatches 0u on Noop, ctx.lightingPass
//          pointer reaches a downstream capture pass
//        - Renderer::lightingEnabled() reports false on default
//          Forward, true after configurePipeline(makeDeferred()),
//          false again after re-configurePipeline(makeDefault())

#include "AYTest.h"
#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"

#include "aymath/MathTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/ForwardOpaquePass.h"
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

#include <AYRenderer.h>
#include <AYRenderTypes.h>

#include <memory>
#include <unordered_map>

using ayt::render::RenderPath;
using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::RenderScene;
using ayt::render::Renderer;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::LightingPass;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPipeline;
using ayt::render::detail::ShadowPass;
using ayt::math::Float4x4;

namespace {

// Capture pass — verifies PassExecContext::lightingPass propagates
// between passes (set on the host → read by downstream passes).
// Records what ctx.lightingPass pointed at, so the test can assert
// it survived the full dispatch without state corruption. Mirrors
// the GBufferCapturePass pattern in Test_B2_GBufferPass.cpp.
struct LightingCapturePass final : public ayt::render::detail::RenderPass {
    static inline const LightingPass* lastSeen = nullptr;
    static inline uint32_t callCount = 0;

    std::string_view name() const override { return "LightingCapture"; }

    uint32_t execute(PassExecContext& ctx) override {
        lastSeen = ctx.lightingPass;
        ++callCount;
        return 0;
    }
};

} // namespace

TEST_SUITE(AYRenderer_B3_LightingForwardDeferred)

TEST_CASE(b3_passexec_context_has_lighting_pass_field_default_null) {
    // §P5 B3 — Pin PassExecContext::lightingPass field exists +
    // default nullptr in 12-field brace-init form. Same guarantee as
    // PR-B2 gbufferPass field default-init (C++14+ trailing-default
    // rule). If anyone tightens the struct to require explicit
    // lightingPass, this compile-fails (along with all the existing
    // 12-/15-field brace-init sites).
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

    CHECK(ctx.lightingPass == nullptr);

    // Setting it the host way still works.
    LightingPass pass;
    ctx.lightingPass = &pass;
    CHECK(ctx.lightingPass == &pass);
}

TEST_CASE(b3_lighting_pass_name_and_initial_state) {
    // Pin LightingPass base contract: name "Lighting", isReady false
    // (no FBO, no program yet — shell only), kLightingViewId == 8
    // (B5 lock per docs/pass-lessons-from-deferred.md §5.1).
    LightingPass pass;
    CHECK(pass.name() == "Lighting");
    CHECK(pass.isReady() == false);
    CHECK(LightingPass::kLightingViewId == 8u);
    CHECK(LightingPass::kLightingDefaultSize == 1280u);
}

TEST_CASE(b3_lighting_pass_noop_uninitialized_returns_zero) {
    // Adapter uninitialized ⇒ execute() returns 0 with no GPU work
    // (mirrors ShadowPass::execute + GBufferPass::execute Noop
    // gates). No crash. Returns to RenderPipeline::executeAll
    // draw-count sum contract intact.
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

    LightingPass pass;
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0u);
    CHECK(pass.isReady() == false);
}

TEST_CASE(b3_lighting_pass_accessors_return_invalid_on_shell) {
    // Stub accessors return BGFX_INVALID_HANDLE / 0 until B5 wires
    // real GPU state. This is the contract downstream consumers
    // (B7+ multi-light, post-Lighting tone-mapping) rely on for
    // "no Lighting FBO this frame → use fallback path".
    LightingPass pass;

    CHECK(bgfx::isValid(pass.lightingFbo()) == false);
    CHECK(bgfx::isValid(pass.lightingOutputFbo()) == false);
    CHECK(pass.lightingWidth() == 0u);
    CHECK(pass.lightingHeight() == 0u);

    // setOutputSize preserves the request but the shell has no FBO
    // yet — accessor still returns invalid handle until B5 lands.
    pass.setOutputSize(1920, 1080);
    CHECK(pass.lightingWidth() == 1920u);
    CHECK(pass.lightingHeight() == 1080u);
    CHECK(bgfx::isValid(pass.lightingFbo()) == false);
}

TEST_CASE(b3_lighting_pass_destroy_resources_is_noop_on_shell) {
    // destroyResources() on empty shell is clean no-op (mirrors
    // GBufferPass::destroyResources shape). B5 wires real FBO +
    // program + uniform buffer then this becomes real cleanup.
    BGFXAdapter adapter;
    LightingPass pass;
    pass.setOutputSize(800, 600);
    pass.destroyResources(adapter);
    CHECK(bgfx::isValid(pass.lightingFbo()) == false);
    CHECK(pass.isReady() == false);
}

TEST_CASE(b3_lighting_pass_pointer_visible_to_following_pass) {
    // LightingCapturePass installed after LightingPass sees
    // ctx.lightingPass == &the lighting instance even though the
    // LightingPass shell has no live FBO. Pins: pointer handoff
    // works regardless of isReady() — same guarantee GBufferPass
    // gets via the B2 suite.
    LightingCapturePass::lastSeen = nullptr;
    LightingCapturePass::callCount = 0;

    RenderPipeline pipe;
    auto extProducer = std::make_unique<LightingPass>();
    LightingPass* const producerPtr = extProducer.get();
    pipe.addPass(std::move(extProducer));
    pipe.addPass(std::make_unique<LightingCapturePass>());

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
    ctx.lightingPass = producerPtr;

    const uint32_t draws = pipe.executeAll(ctx);
    CHECK(draws == 0u);
    CHECK(LightingCapturePass::callCount == 1u);
    CHECK(LightingCapturePass::lastSeen == producerPtr);
}

TEST_CASE(b3_full_deferred_pipeline_noop_dispatch_with_lighting_pointer_wired) {
    // End-to-end Forward/Deferred factory-layer integration case:
    //   1. makeDefault() returns 5-slot Forward (Shadow + FO + Trans
    //      + PP + UI) — pins Forward path 0 behavior change (the B2
    //      cutsheet promise).
    //   2. makeDeferred() returns 6-slot Deferred (Shadow + GBuffer
    //      + Lighting + Trans + PP + UI) — pins ForwardOpaque OMIT
    //      from Deferred list per §4.1 red line #4.
    //   3. Manual pipeline {Shadow, GBuffer, Lighting, Trans, PP, UI}
    //      dispatch returns 0u on Noop, no crash, ctx.lightingPass
    //      pointer reaches a downstream capture pass.
    //   4. Renderer::lightingEnabled() reports false on default
    //      Forward (no Lighting slot mounted); true after
    //      configurePipeline(makeDeferred()).
    //
    // We deliberately omit ForwardOpaquePass from the manual pipeline
    // (same workaround as Test_B2_GBufferPass::test 7) because it
    // calls bgfx C-API surface (setViewTransform / setViewFrameBuffer)
    // without an isInitialized() guard — pre-existing FO bug,
    // orthogonal to B3.

    // Part 1: Forward default — 8 slots (S1a 2026-07-23 added
    // BloomExtract between Transparent and PostProcess; S1b
    // 2026-07-23 added BloomBlur between BloomExtract and
    // PostProcess; S4b 2026-07-23 added DepthHaze between
    // BloomBlur and PostProcess), no GBuffer, no Lighting.
    const RenderPipelineDesc def = RenderPipelineDesc::makeDefault();
    CHECK(def.path == RenderPath::Forward);
    CHECK(def.passes.size() == 9u);
    CHECK(def.contains(RenderPassSlot::Shadow));
    CHECK(def.contains(RenderPassSlot::ForwardOpaque));
    CHECK(def.contains(RenderPassSlot::Transparent));
    CHECK(def.contains(RenderPassSlot::BloomExtract));   // S1a (2026-07-23)
    CHECK(def.contains(RenderPassSlot::BloomBlur));      // S1b (2026-07-23)
    CHECK(def.contains(RenderPassSlot::DepthHaze));      // S4b (2026-07-23)
    CHECK(def.contains(RenderPassSlot::PostProcess));
    CHECK(def.contains(RenderPassSlot::UI));
    CHECK(!def.contains(RenderPassSlot::GBuffer));   // B3: not in Forward
    CHECK(!def.contains(RenderPassSlot::Lighting));  // B3: not in Forward
    CHECK(!def.contains(RenderPassSlot::Skybox));    // §Skybox0: Skybox not in Forward

    // Part 2: Deferred factory — 10 slots (§Skybox0 2026-07-23 added
    // Skybox between Shadow and GBuffer; S1a 2026-07-23 added
    // BloomExtract between Transparent and PostProcess; S1b
    // 2026-07-23 added BloomBlur between BloomExtract and
    // PostProcess; S4b 2026-07-23 added DepthHaze between
    // BloomBlur and PostProcess). GBuffer + Lighting + Skybox added,
    // ForwardOpaque OMITTED.
    const RenderPipelineDesc deferred = RenderPipelineDesc::makeDeferred();
    CHECK(deferred.path == RenderPath::Deferred);
    CHECK(deferred.passes.size() == 12u);   // §A2 SSAO MVP (2026-07-24): +1 SSAO between DepthHaze and PostProcess; V1 GBuffer Debug (2026-07-24): +1 GBufferDebug appended last
    CHECK(deferred.contains(RenderPassSlot::Shadow));
    CHECK(deferred.contains(RenderPassSlot::Skybox));    // §Skybox0: in Deferred
    CHECK(deferred.contains(RenderPassSlot::GBuffer));   // B3: in Deferred
    CHECK(deferred.contains(RenderPassSlot::Lighting));  // B3: in Deferred
    CHECK(deferred.contains(RenderPassSlot::Transparent));
    CHECK(deferred.contains(RenderPassSlot::BloomExtract));   // S1a (2026-07-23)
    CHECK(deferred.contains(RenderPassSlot::BloomBlur));      // S1b (2026-07-23)
    CHECK(deferred.contains(RenderPassSlot::DepthHaze));      // S4b (2026-07-23)
    CHECK(deferred.contains(RenderPassSlot::PostProcess));
    CHECK(deferred.contains(RenderPassSlot::UI));
    CHECK(!deferred.contains(RenderPassSlot::ForwardOpaque)); // B3: OMIT per §4.1 red line #4

    // Part 3: Manual Deferred dispatch — pointer propagates.
    //
    // We deliberately omit BOTH ForwardOpaquePass AND
    // TransparentPass from the manual pipeline (same workaround
    // as Test_B2_GBufferPass::test 7) because they call bgfx C-API
    // surface (setViewTransform / setViewFrameBuffer) without an
    // isInitialized() guard — pre-existing FO/Trans bug, orthogonal
    // to B3. Mirrors Test_B2::b2_full_pipeline_noop_dispatch exactly.
    // The GBuffer + Lighting + Shadow + PostProcess + UI chain is
    // sufficient to pin the B3 contract: lightingPass pointer
    // survives full dispatch through executeAll() without crash
    // and reaches the downstream capture pass.
    LightingCapturePass::lastSeen = nullptr;
    LightingCapturePass::callCount = 0;

    RenderPipeline pipe;
    auto extProducer = std::make_unique<LightingPass>();
    LightingPass* const producerPtr = extProducer.get();
    pipe.addPass(std::make_unique<ShadowPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::GBufferPass>());
    pipe.addPass(std::move(extProducer));
    pipe.addPass(std::make_unique<ayt::render::detail::PostProcessPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::UIPass>());
    pipe.addPass(std::make_unique<LightingCapturePass>());

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
    ctx.lightingPass = producerPtr;

    const uint32_t total = pipe.executeAll(ctx);
    CHECK(total == 0u);

    CHECK(pipe.passes().size() == 6u);  // Shadow + GBuffer + Lighting + PP + UI + LightingCapture
    CHECK(pipe.passes()[0]->name() == "Shadow");
    CHECK(pipe.passes()[1]->name() == "GBuffer");
    CHECK(pipe.passes()[2]->name() == "Lighting");
    CHECK(pipe.passes()[3]->name() == "PostProcess");
    CHECK(pipe.passes()[4]->name() == "UI");
    CHECK(pipe.passes()[5]->name() == "LightingCapture");
    CHECK(LightingCapturePass::callCount == 1u);
    CHECK(LightingCapturePass::lastSeen == producerPtr);

    // Part 4: lightingEnabled() public API — Forward default false,
    // Deferred true.
    Renderer renderer;
    CHECK(renderer.lightingEnabled() == false);  // default Forward
    renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
    CHECK(renderer.lightingEnabled() == true);   // after Deferred config
    renderer.configurePipeline(RenderPipelineDesc::makeDefault());
    CHECK(renderer.lightingEnabled() == false);  // back to Forward
}

TEST_SUITE_END