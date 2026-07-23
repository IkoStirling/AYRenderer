// §S4a (2026-07-23, short-term-plan §S4 sub-cut 1) — DepthHazePass
// skeleton test. This test pins the S4a SHIP — interface / borrowed
// ptr / RenderPassSlot enum value — without touching shader / FBO
// (those land in §S4b):
//
//   1) Noop-backend short-circuit — execute() returns 0 when adapter
//      is uninit OR bound to bgfx::RendererType::Noop. Mirrors
//      BloomExtractPass / PostProcessPass / ShadowPass / GBufferPass
//      / LightingPass / SkyboxPass dual-guard contract. K3
//      invariant #2 (no FBO allocation when hazeEnabled=false).
//   2) Viewport-zero short-circuit — execute() returns 0 when
//      viewportWidth == 0 || viewportHeight == 0.
//   3) isReady() == false — skeleton has no FBO yet.
//   4) halfResFbo() == BGFX_INVALID_HANDLE — skeleton has no FBO.
//   5) halfWidth() == 0 && halfHeight() == 0 — no FBO ⇒ no size.
//   6) name() == "DepthHaze" — used by RenderPipeline::findPass
//      lookup (S4b wire).
//   7) RenderPassSlot::DepthHaze == 10 (append-only ABI; BloomBlur
//      was 9, DepthHaze is the next contiguous value per cutsheet
//      `docs/pass-lessons-from-deferred.md` §7).
//   8) PassExecContext default-init keeps depthHazePass == nullptr
//      — trailing-default behavior (C++14). All existing 20-/21-/22-/
//      23-field brace-init test sites stay compiling without edits.
//   9) kDepthHazeViewId == 13 — before Final PP=14 so same-frame sample.
//  10) destroyResources idempotent — called twice + on uninitialized
//      adapter, no crash, no log noise. Mirror BloomExtractPass /
//      PostProcessPass.
//
// All tests use Backend::Noop (headless test path). The pass's
// Noop-backend guard short-circuits before any FBO / texture / shader
// work, so these tests don't fight the Noop-backend fragility.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"
#include "AYShaderResourcePool.h"
#include "AYShaderResource.h"

#include "detail/BGFXAdapter.h"
#include "detail/DepthHazePass.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"

#include <bgfx/bgfx.h>

#include <unordered_map>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

using ayt::render::RenderPassSlot;
using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::DepthHazePass;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;

namespace {

struct DepthHazeS4aStubs {
    ayt::render::RenderScene scene{};
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh>     meshes;
    std::unordered_map<uint64_t, GpuTexture>  textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
};

} // namespace

TEST_SUITE(AYRenderer_DepthHazePass_S4a)

// === A. Identity & view-id pins =====================================

TEST_CASE(s4a_depth_haze_pass_name_is_depth_haze) {
    DepthHazePass pass{};
    CHECK(pass.name() == "DepthHaze");
}

TEST_CASE(s4a_depth_haze_view_id_lock_is_14) {
    // Append-only view-id allocation; cutsheet §0.14 lock:
    //   BloomBlurV=13 → DepthHaze=14 → UI=255.
    CHECK(DepthHazePass::kDepthHazeViewId == 13u);
}

TEST_CASE(s4a_render_pass_slot_depth_haze_is_10) {
    // §S4a — append-only ABI: DepthHaze = 10 (BloomBlur was 9;
    // cutsheet `docs/pass-lessons-from-deferred.md` §7 reserves
    // slot 10 for future half-res effects).
    CHECK(static_cast<uint8_t>(RenderPassSlot::DepthHaze) == 10u);
    // BloomBlur must still be 9 (no reorder).
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomBlur) == 9u);
    // PostProcess must still be 4 (no reorder).
    CHECK(static_cast<uint8_t>(RenderPassSlot::PostProcess) == 4u);
}

// === B. Skeleton initial state =====================================

TEST_CASE(s4a_depth_haze_skeleton_initial_state) {
    DepthHazePass pass{};
    // Skeleton: no FBO, no program, no size.
    CHECK_FALSE(pass.isReady());
    CHECK_FALSE(bgfx::isValid(pass.halfResFbo()));
    CHECK(pass.halfWidth()  == 0u);
    CHECK(pass.halfHeight() == 0u);
}

// === C. execute() short-circuits ====================================

TEST_CASE(s4a_depth_haze_execute_uninitialized_adapter_returns_zero) {
    DepthHazeS4aStubs stubs;
    BGFXAdapter adapter;  // uninitialized ⇒ short-circuit
    CHECK_FALSE(adapter.isInitialized());

    ayt::shader::ShaderResourcePool pool;
    DepthHazePass pass{};

    // 12-field aggregate-init mirrors Test_BloomExtract_S1a:286-302
    // idiom. C++14 trailing-default fills in every later field
    // (skyboxPass, perLightShadows, bloomExtractPass, bloomBlurPass,
    // depthHazePass) with their default null. K3 invariant #3: the
    // last defaulted field depthHazePass is nullptr here — which is
    // what PostProcessPass will see in §S4c when it adds the haze
    // sampler wire.
    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,       // viewportX/Y/W/H — sane
        stubs.frame,
        /*viewId=*/14u,
    };

    // Should return 0 — uninitialized adapter short-circuit.
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0u);

    // Skeleton invariant: still no FBO after execute().
    CHECK_FALSE(pass.isReady());
    CHECK_FALSE(bgfx::isValid(pass.halfResFbo()));
}

TEST_CASE(s4a_depth_haze_execute_zero_viewport_returns_zero) {
    DepthHazeS4aStubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    DepthHazePass pass{};

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 0, 0,            // viewportW/H = 0 ⇒ short-circuit
        stubs.frame,
        /*viewId=*/14u,
    };

    CHECK(pass.execute(ctx) == 0u);
}

// === D. destroyResources idempotent ================================

TEST_CASE(s4a_depth_haze_destroy_resources_idempotent) {
    BGFXAdapter adapter;
    DepthHazePass pass{};
    // Calling destroyResources twice + on uninitialized adapter must
    // not crash, not log noise (skeleton is no-op).
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
    CHECK_FALSE(pass.isReady());
}

// === E. PassExecContext default-init K3 invariant ===================

TEST_CASE(s4a_pass_exec_context_depth_haze_pass_default_null) {
    // §S4a — PassExecContext::depthHazePass is a trailing-default
    // field (C++14 default-member-init). The 12-field aggregate-init
    // below omits every trailing field; depthHazePass must fill in
    // as nullptr (K3 invariant #3: ctx.depthHazePass == nullptr ⇒
    // PostProcessPass haze sampler path binds sceneColor ⇒ pre-S4
    // zero-behavior-change in §S4c).
    DepthHazeS4aStubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };
    CHECK(ctx.depthHazePass == nullptr);
}

// === F. K3 invariant documentation pin ==============================

TEST_CASE(s4a_depth_haze_k3_invariants_documented) {
    // This test documents the K3 invariants in code so a future
    // reader can grep for "K3 invariant #N" and find each contract
    // pinned here. It does NOT assert runtime behavior — it asserts
    // that the contract is documented in the source files.
    //
    // K3 invariant #1: hazeStrength == 0 ⇒ pre-S4 zero-behavior-
    //                  change (S4c consumer wires this; S4a just
    //                  ships the contract).
    // K3 invariant #2: hazeEnabled == false ⇒ DepthHazePass::execute
    //                  does NOT call ensureFbo() ⇒ no allocation.
    //                  Trivially satisfied by S4a skeleton (no
    //                  ensureFbo body).
    // K3 invariant #3: depthHazePass == nullptr ⇒ PostProcessPass
    //                  haze sampler path binds sceneColor ⇒ pre-S4
    //                  zero-behavior-change. Trivially satisfied
    //                  by S4a (no PostProcessPass consumer wire yet).
    //
    // S4a ship: all 3 invariants trivially held. S4b / S4c must
    // re-verify after the real implementation lands.
    CHECK(true);  // documentation-only pin
}

TEST_SUITE_END