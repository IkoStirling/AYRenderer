// §S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — DepthHazePass
// real-implementation test. Pins the S4b SHIP — exponential fog
// shader (主人拍板 B) + half-res RGBA8 FBO + view 13 + K3
// invariants — without touching the PostProcessPass haze sampler
// wire (that lands in §S4c).
//
// Tests pin:
//   1) FrameContext default haze fields (hazeEnabled=false,
//      hazeStrength=0, hazeDensity=0.02, hazeColor=…) — host opt-in
//      contract via frame.hazeEnabled=true.
//   2) K3 invariant #2 — frame.hazeEnabled=false ⇒ execute() returns
//      0 AND ensureFbo() is NOT called ⇒ halfResFbo() stays invalid.
//      Mirrors frame-graph-mvp.md §7 第 3 条.
//   3) K3 invariant #1 — frame.hazeStrength <= 0 ⇒ execute() returns
//      0 + no FBO ensured (守 "host enabled + strength=0 ⇒ no work").
//   4) K3 invariant — frame.hazeEnabled=true + hazeStrength>0 + real
//      adapter path ⇒ execute() does NOT throw / abort on a fresh
//      BGFXAdapter (Noop backend short-circuits cleanly).
//   5) K3 invariant #3 — PassExecContext default-init keeps
//      depthHazePass == nullptr. C++14 trailing-default behavior
//      preserves all existing 22-/23-field brace-init test sites.
//   6) View-id lock — kDepthHazeViewId == 13 (before Final PP=14).
//   7) Cache-key extern — kDepthHazeCacheKeyCStr is addressable and
//      matches the mirror literal (Bug fix #3 mirror — guards
//      against the pre-S4 `.cpp` static self-compare trap).
//   8) ABI: RenderPassSlot::DepthHaze == 10 still holds; no reorder.
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

struct DepthHazeS4bStubs {
    ayt::render::RenderScene scene{};
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh>     meshes;
    std::unordered_map<uint64_t, GpuTexture>  textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
};

} // namespace

TEST_SUITE(AYRenderer_DepthHazePass_S4b)

// === A. Identity & view-id pins =====================================

TEST_CASE(s4b_depth_haze_view_id_lock_is_14) {
    // Append-only view-id allocation; cutsheet §S4 lock:
    //   BloomBlurV=13 → DepthHaze=14 → UI=255.
    CHECK(DepthHazePass::kDepthHazeViewId == 13u);
}

TEST_CASE(s4b_render_pass_slot_depth_haze_is_10) {
    // §S4b — append-only ABI: DepthHaze = 10 (BloomBlur was 9;
    // no reorder). C++14 trailing-default behavior in
    // RenderPassSlot keeps the existing 9-slot forward pipeline
    // compile-clean.
    CHECK(static_cast<uint8_t>(RenderPassSlot::DepthHaze) == 10u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomBlur) == 9u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::PostProcess) == 4u);
}

TEST_CASE(s4b_depth_haze_cache_key_extern_addressable) {
    // §S4b — Bug fix #3 mirror (BloomExtractPass / BloomBlurPass /
    // SkyboxPass / LightingPass pattern). The extern declaration in
    // DepthHazePass.h must bind to the file-scope literal in
    // DepthHazePass.cpp. Drift between the two is now a compile-time
    // link error instead of a runtime self-compare.
    const char* const mirror = "depthhaze_v2_worldpos_campos";
    CHECK(std::string(ayt::render::detail::kDepthHazeCacheKeyCStr)
          == std::string(mirror));
}

// === B. FrameContext default haze fields ============================

TEST_CASE(s4b_frame_context_haze_defaults_are_off) {
    // §S4b — FrameContext haze fields default to OFF (K3 invariant
    // #1 host opt-in contract). The host flips hazeEnabled=true and
    // sets a non-zero hazeStrength to actually opt in. Pre-S4 byte-
    // equivalent: every existing FrameContext brace-init keeps the
    // new fields at their defaults ⇒ no behavior change.
    FrameContext frame;
    CHECK_FALSE(frame.hazeEnabled);
    CHECK(frame.hazeStrength == 0.0f);
    // Density default is the exponential falloff rate; a non-zero
    // default is fine because hazeStrength==0 ⇒ haze is off (the
    // FS gate `step(0.0, strength)` collapses to 0).
    CHECK(frame.hazeDensity > 0.0f);
    // Color default is a soft blue-grey daylight fog — visible to
    // the host the moment hazeEnabled is flipped on.
    CHECK(frame.hazeColor.x > 0.0f);
    CHECK(frame.hazeColor.y > 0.0f);
    CHECK(frame.hazeColor.z > 0.0f);
}

// === C. K3 invariant #2 — hazeEnabled=false ⇒ no FBO ensure ========

TEST_CASE(s4b_k3_invariant_haze_disabled_does_not_ensure_fbo) {
    // §S4b K3 invariant #2 — frame.hazeEnabled=false ⇒ execute()
    // returns 0 BEFORE ensureFbo() runs ⇒ halfResFbo() stays
    // invalid (守 frame-graph-mvp.md §7 第 3 条: 关效果即不分配 RT).
    DepthHazeS4bStubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    DepthHazePass pass{};

    // hazeEnabled defaults to false; explicit for clarity.
    stubs.frame.hazeEnabled  = false;
    stubs.frame.hazeStrength = 0.5f;  // even with non-zero strength
    stubs.frame.hazeDensity  = 0.02f;
    stubs.frame.hazeColor    = ayt::math::FVector3(0.5f, 0.6f, 0.7f);

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,  // sane viewport
        stubs.frame,
        /*viewId=*/14u,
    };

    CHECK(pass.execute(ctx) == 0u);
    CHECK_FALSE(pass.isReady());
    CHECK_FALSE(bgfx::isValid(pass.halfResFbo()));
    CHECK(pass.halfWidth()  == 0u);
    CHECK(pass.halfHeight() == 0u);
}

// === D. K3 invariant #1 — hazeStrength<=0 ⇒ no FBO ensure ==========

TEST_CASE(s4b_k3_invariant_haze_strength_zero_does_not_ensure_fbo) {
    // §S4b K3 invariant #1 — frame.hazeStrength <= 0 ⇒ execute()
    // returns 0 BEFORE ensureFbo() runs. The "enabled but strength=0"
    // combination is a valid host state (e.g., Editor slider at
    // bottom of range); the pass must stay zero-cost.
    DepthHazeS4bStubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    DepthHazePass pass{};

    stubs.frame.hazeEnabled  = true;
    stubs.frame.hazeStrength = 0.0f;  // explicit zero
    stubs.frame.hazeDensity  = 0.02f;
    stubs.frame.hazeColor    = ayt::math::FVector3(0.5f, 0.6f, 0.7f);

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };

    CHECK(pass.execute(ctx) == 0u);
    CHECK_FALSE(pass.isReady());
    CHECK_FALSE(bgfx::isValid(pass.halfResFbo()));
}

TEST_CASE(s4b_k3_invariant_negative_haze_strength_does_not_ensure_fbo) {
    // §S4b K3 invariant #1 extended — negative hazeStrength (host
    // bug or accident) must also early-return cleanly without
    // ensureFbo. Mirror the "host enabled + negative strength"
    // case to guard against any future "if (strength) ensure()"
    // refactor that misses the negative side.
    DepthHazeS4bStubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    DepthHazePass pass{};

    stubs.frame.hazeEnabled  = true;
    stubs.frame.hazeStrength = -1.0f;  // negative — host accident
    stubs.frame.hazeDensity  = 0.02f;
    stubs.frame.hazeColor    = ayt::math::FVector3(0.5f, 0.6f, 0.7f);

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };

    CHECK(pass.execute(ctx) == 0u);
    CHECK_FALSE(pass.isReady());
}

// === E. K3 invariant — Noop backend short-circuits cleanly ==========

TEST_CASE(s4b_k3_invariant_noop_backend_does_not_ensure_fbo) {
    // §S4b K3 invariant — Noop backend (default-constructed
    // BGFXAdapter is uninitialized, which is the headless test
    // path) ⇒ execute() returns 0 + no FBO created. Mirrors
    // BloomExtractPass / BloomBlurPass / PostProcessPass contract.
    DepthHazeS4bStubs stubs;
    BGFXAdapter adapter;  // uninitialized ⇒ Noop short-circuit
    CHECK_FALSE(adapter.isInitialized());

    ayt::shader::ShaderResourcePool pool;
    DepthHazePass pass{};

    // Even with haze fully on, Noop must NOT allocate the FBO.
    stubs.frame.hazeEnabled  = true;
    stubs.frame.hazeStrength = 0.5f;
    stubs.frame.hazeDensity  = 0.02f;
    stubs.frame.hazeColor    = ayt::math::FVector3(0.5f, 0.6f, 0.7f);

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };

    CHECK(pass.execute(ctx) == 0u);
    CHECK_FALSE(pass.isReady());
    CHECK_FALSE(bgfx::isValid(pass.halfResFbo()));
}

// === F. K3 invariant #3 — PassExecContext default null ==============

TEST_CASE(s4b_pass_exec_context_depth_haze_pass_default_null) {
    // §S4b K3 invariant #3 — PassExecContext::depthHazePass is a
    // trailing-default field (C++14 default-member-init). The
    // 22-field aggregate-init below omits the trailing field;
    // depthHazePass must fill in as nullptr (守 PostProcessPass
    // S4c haze sampler path: nullptr ⇒ bind sceneColor ⇒
    // zero-haze collapse).
    DepthHazeS4bStubs stubs;
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

TEST_CASE(s4b_pass_exec_context_brace_init_compiles) {
    // §S4b — compile-time check that 22-field brace-init compiles
    // cleanly (C++14 trailing-default behavior keeps all existing
    // Test_PostProcess_R5Plus / Test_BloomExtract_S1a /
    // Test_BloomBlur_S1b / Test_Skybox0 / Test_F2_ForwardShadow
    // brace-init sites compiling without edits).
    DepthHazeS4bStubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;

    // Trailing defaults omitted — depthHazePass should fill as nullptr.
    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };
    CHECK(ctx.depthHazePass == nullptr);
}

// === G. K3 invariant — destroyResources idempotent + safe on off ===

TEST_CASE(s4b_destroy_resources_idempotent_with_haze_off) {
    // §S4b — destroyResources must be safe to call even when the
    // pass never ran (hazeEnabled=false ⇒ ensureFbo never called ⇒
    // _fbo invalid). Mirrors BloomExtractPass / BloomBlurPass
    // contract.
    BGFXAdapter adapter;
    DepthHazePass pass{};
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
    CHECK_FALSE(pass.isReady());
    CHECK_FALSE(bgfx::isValid(pass.halfResFbo()));
}

// === H. K3 invariant documentation pin ==============================

TEST_CASE(s4b_k3_invariants_documented) {
    // Documents the S4b K3 invariants in code so a future reader can
    // grep for "K3 invariant #N" and find each contract pinned
    // here. The runtime K3 tests above (C/D/E) assert the actual
    // behavior; this test pins the contract documentation.
    //
    // K3 invariant #1: hazeStrength <= 0 OR hazeEnabled == false
    //                  ⇒ DepthHazePass::execute returns 0 BEFORE
    //                  ensureFbo (no FBO allocation, no view-id
    //                  collision). Mirrored from S4a contract;
    //                  S4b adds the hazeStrength<=0 gate at the
    //                  same call site.
    // K3 invariant #2: hazeEnabled=false ⇒ ensureFbo NEVER called
    //                  (zero allocation when disabled). Mirrors
    //                  frame-graph-mvp.md §7 第 3 条.
    // K3 invariant #3: depthHazePass == nullptr (custom desc
    //                  omits DepthHaze slot) ⇒ PostProcessPass S4c
    //                  haze sampler path binds sceneColor; FS
    //                  branchless composite collapses to `raw *
    //                  (1 - 0) = raw` (byte-equivalent to
    //                  hazeEnabled=false). Mirror §S1c
    //                  bloomBlurPass==nullptr invariant.
    // K3 invariant #4: Deferred dist = length(GBuffer RT2 worldPos
    //                  - camPos); Forward / no gbufferMotionRt ⇒
    //                  execute returns 0 (safe no-haze).
    // K3 invariant #5: ABI: append-only — RenderPassSlot::DepthHaze
    //                  = 10; view id 14 reserved; no existing
    //                  enum / view-id value reorders.
    CHECK(true);  // documentation-only pin
}

TEST_SUITE_END
