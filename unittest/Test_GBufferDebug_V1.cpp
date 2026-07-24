// V1 GBuffer Debug skeleton test (2026-07-24). Pins the SHIP
// contract for V1:
//
//   1) RenderPassSlot enum ABI:
//        SSAO = 11 (unchanged by append)
//        ⇒ GBufferDebug = 12 (append-only; SSAO max was 11)
//   2) View id reservation lock:
//        GBufferDebugPass::kGBufferDebugViewId == 250
//        (verified unused via repo grep, 2026-07-24)
//   3) FrameContext default state (K-GBD-1 zero alloc):
//        gbufferDebugEnabled == false, gbufferDebugChannel == 0
//        ⇒ host central gate false ⇒ FBO not created ⇒ zero alloc
//   4) GBufferDebugChannel enum completeness (5 channels; WorldPos
//      and Motion alias gbufferMotionRt() in V1; K-GBD-3).
//   5) Cache-key extern mirror (Bug fix #3 pattern):
//        kGBufferDebugCacheKeyCStr literals must agree between
//        .h declaration + .cpp definition. Pre-V2 the literal is
//        a placeholder; V2 bumps it. Drift detection guard.
//   6) GBufferDebugPass skeleton execute() returns 0 — even on
//        non-initialized / Noop adapter short-circuits, and
//        when the central gate fires but the FBO is invalid.
//   7) GBufferDebugPass::isReady() returns false (V1 stub; V2 lifts).
//   8) GBufferDebugPass::destroyResources() idempotent.
//   9) makeDefault() does NOT contain GBufferDebug (Forward no-op).
//  10) makeDeferred() DOES contain GBufferDebug (and is the LAST
//        slot so bgfx ascending view-id dispatch runs view 250
//        strictly after view 15).
//  11) PassExecContext::gbufferDebugFbo defaults to INVALID
//        (trailing-default ABI-lock; all 23-field brace-init
//        test sites keep compiling without edits).
//
// All tests use Backend::Noop (headless test path). The pass's
// Noop-backend / uninit-adapter guards short-circuit before any
// real GPU work, so these tests don't fight Noop fragility.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"
#include "AYShaderResourcePool.h"
#include "AYShaderResource.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GBufferDebugPass.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <unordered_map>

using ayt::render::RenderPassSlot;
using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GBufferDebugChannel;
using ayt::render::detail::GBufferDebugPass;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;

namespace {

struct GBufferDebugV1Stubs {
    BGFXAdapter adapter{};
    ayt::shader::ShaderResourcePool pool{};
    ayt::render::RenderScene scene{};
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh>     meshes;
    std::unordered_map<uint64_t, GpuTexture>  textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
};

} // namespace

TEST_SUITE(AYRenderer_GBufferDebug_V1)

// ─── A. RenderPassSlot enum ABI lock ───────────────────────────────

TEST_CASE(v1_render_pass_slot_gbufferdebug_is_12_append_only) {
    // V1: SSAO max was 11; GBufferDebug appends as 12. Forward +
    // earlier slots unchanged.
    CHECK(static_cast<uint8_t>(RenderPassSlot::GBufferDebug) == 12u);
    // Regression: prior slot values unchanged (cutsheet §S2 ABI lock).
    CHECK(static_cast<uint8_t>(RenderPassSlot::SSAO)         == 11u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::DepthHaze)    == 10u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomBlur)    ==  9u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomExtract) ==  8u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::PostProcess)  ==  4u);
}

TEST_CASE(v1_view_id_lock_is_250) {
    // V1 view-map lock. Verified unused across repo (2026-07-24
    // `grep -w 250` → 0 hits). Below 255=UI-Editor, above 0..15
    // main-frame stream.
    CHECK(GBufferDebugPass::kGBufferDebugViewId == 250u);
}

// ─── B. GBufferDebugChannel enum completeness ──────────────────────

TEST_CASE(v1_channel_enum_completeness_5_values) {
    // V1 ships 5 logical channels; Count sentinel pins the end.
    // WorldPos(2) + Motion(3) alias gbufferMotionRt() until V2
    // splits a real motion RT (K-GBD-3).
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Albedo)   == 0u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Normal)   == 1u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::WorldPos) == 2u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Motion)   == 3u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Depth)    == 4u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Count)    == 5u);
    CHECK(GBufferDebugPass::kGBufferDebugChannelCount == 5u);
}

// ─── C. FrameContext default state (K-GBD-1 zero alloc) ───────────

TEST_CASE(v1_frame_context_debug_defaults_off) {
    // K-GBD-1: default OFF means render() central gate is false ⇒
    // FBO not created ⇒ zero alloc ⇒ execute returns 0.
    FrameContext ctx;
    CHECK_FALSE(ctx.gbufferDebugEnabled);
    CHECK(ctx.gbufferDebugChannel == 0u);
    // Regression: SSAO tail from §S2 unchanged.
    CHECK_FALSE(ctx.ssaoEnabled);
    CHECK(ctx.ssaoStrength == 0.0f);
    CHECK(ctx.ssaoRadius   == 0.5f);
    CHECK(ctx.ssaoBias     == 0.025f);
}

TEST_CASE(v1_frame_context_debug_round_trip) {
    // V1: host can flip the knob at the start of a frame and
    // FrameContext survives the copy (mirror SSAO A1 round-trip
    // test pattern).
    FrameContext ctx;
    ctx.gbufferDebugEnabled = true;
    ctx.gbufferDebugChannel = 3u;  // Motion
    CHECK(ctx.gbufferDebugEnabled);
    CHECK(ctx.gbufferDebugChannel == 3u);
}

// ─── D. Cache-key extern mirror (Bug fix #3) ──────────────────────

TEST_CASE(v1_cache_key_extern_mirror_non_null) {
    CHECK(ayt::render::detail::kGBufferDebugCacheKeyCStr != nullptr);
    const std::size_t len = std::char_traits<char>::length(
        ayt::render::detail::kGBufferDebugCacheKeyCStr);
    CHECK(len > 0);
}

TEST_CASE(v1_cache_key_extern_mirror_contains_marker) {
    // V1 placeholder literal contains "gbufferdebug" + version
    // stamp "v1" so a future reader searching for the marker
    // can find this contract. V2 bumps the version to "v2".
    const std::string key(ayt::render::detail::kGBufferDebugCacheKeyCStr);
    CHECK(key.find("gbufferdebug") != std::string::npos);
    CHECK(key.find("v1")          != std::string::npos);
}

// ─── E. Skeleton initial state + destroyResources idempotency ─────

TEST_CASE(v1_pass_skeleton_initial_state) {
    GBufferDebugPass pass{};
    CHECK(pass.name() == "GBufferDebug");
    CHECK_FALSE(pass.isReady());
}

TEST_CASE(v1_pass_destroy_resources_idempotent) {
    // V1: pass holds no real GPU resources. destroyResources must
    // be safe to call on an uninitialized adapter and idempotent
    // across repeated calls (mirror SSAO A1 pattern).
    BGFXAdapter adapter;
    GBufferDebugPass pass{};
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
    CHECK_FALSE(pass.isReady());
}

TEST_CASE(v1_pass_isReady_stub_returns_false) {
    // V1 isReady() returns _program.isValid(). The program is
    // never acquired (V1 ships no real Phoskia source) so
    // isReady() must remain false even after execute() runs
    // (the V1 path always early-returns 0 before program is
    // touched, so isReady() cannot flip).
    GBufferDebugPass pass{};
    CHECK_FALSE(pass.isReady());
    // V1 short-circuits at step-4 (target invalid) ⇒ execute()
    // never touches the program state.
    GBufferDebugV1Stubs stubs;
    PassExecContext ctx{
        stubs.adapter,
        stubs.pool,
        stubs.scene,
        stubs.meshes,
        stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        0u,
    };
    ctx.gbufferDebugFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    CHECK(pass.execute(ctx) == 0u);
    CHECK_FALSE(pass.isReady());
}

// ─── F. execute() short-circuit ladder (K-GBD-1) ──────────────────

TEST_CASE(v1_execute_uninitialized_adapter_returns_zero) {
    // K-GBD-1 step-1: adapter not initialized ⇒ 0 draw + 0 alloc.
    GBufferDebugPass pass{};
    GBufferDebugV1Stubs stubs;
    PassExecContext ctx{
        stubs.adapter,
        stubs.pool,
        stubs.scene,
        stubs.meshes,
        stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/0u,
    };
    ctx.gbufferDebugFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    CHECK(pass.execute(ctx) == 0u);
}

TEST_CASE(v1_execute_no_target_fbo_returns_zero) {
    // K-GBD-1 step-4: ctx.gbufferDebugFbo INVALID ⇒ 0 draw + 0 alloc.
    // This is the central-gate closed case (host did not enable
    // GBufferDebug ⇒ central gate false ⇒ FBO never created ⇒
    // ctx field INVALID ⇒ execute early-returns 0).
    GBufferDebugPass pass{};
    GBufferDebugV1Stubs stubs;
    PassExecContext ctx{
        stubs.adapter,
        stubs.pool,
        stubs.scene,
        stubs.meshes,
        stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        0u,
    };
    ctx.gbufferDebugFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    CHECK(pass.execute(ctx) == 0u);
}

TEST_CASE(v1_execute_no_gbuffer_pass_returns_zero) {
    // K-GBD-1 step-5: ctx.gbufferPass == nullptr (e.g. Forward
    // pipeline) ⇒ 0 draw. The central gate also gates on this,
    // so the ctx field is normally INVALID for Forward hosts;
    // the step-5 check is a belt-and-braces double-check.
    GBufferDebugPass pass{};
    GBufferDebugV1Stubs stubs;
    PassExecContext ctx{
        stubs.adapter,
        stubs.pool,
        stubs.scene,
        stubs.meshes,
        stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        0u,
    };
    // Forge a "valid" FBO so we get past step-4 and exercise the
    // step-5 double-check.
    ctx.gbufferDebugFbo.idx = 1u;  // non-INVALID sentinel
    ctx.gbufferPass = nullptr;
    CHECK(pass.execute(ctx) == 0u);
}

// ─── G. Pipeline slot placement (K-GBD invariant: deferred-only,
//      makeDefault() omits, makeDeferred() appends LAST) ──────────

TEST_CASE(v1_make_default_does_not_contain_gbufferdebug_slot) {
    // K-GBD: Forward pipeline must NEVER see the GBufferDebug
    // pass. The slot's host knob is forward-safe (gbufferPassPtr
    // == nullptr ⇒ gate false), but the pass itself should not
    // even be in the slot list (Forward has no GBuffer to debug).
    const auto desc = ayt::render::RenderPipelineDesc::makeDefault();
    CHECK_FALSE(desc.contains(RenderPassSlot::GBufferDebug));
}

TEST_CASE(v1_make_deferred_contains_gbufferdebug_slot_last) {
    // K-GBD: Deferred pipeline contains the slot AND it is the
    // LAST slot in the list. The "last" placement is what keeps
    // the bgfx ascending view-id dispatch running view 250
    // strictly after view 15, preserving the 0..15 main-frame
    // stream byte-equivalence (cutsheet §G1 V1 red line).
    const auto desc = ayt::render::RenderPipelineDesc::makeDeferred();
    CHECK(desc.contains(RenderPassSlot::GBufferDebug));
    CHECK_FALSE(desc.passes.empty());
    CHECK(desc.passes.back() == RenderPassSlot::GBufferDebug);
}

TEST_CASE(v1_make_deferred_total_slot_count_incremented_by_one) {
    // Regression: appending GBufferDebug increments the
    // makeDeferred slot count by 1 vs pre-V1. Pre-V1 was
    // 10 slots (Shadow/Skybox/GBuffer/Lighting/Transparent/
    // BloomExtract/BloomBlur/DepthHaze/SSAO/PostProcess/UI).
    // Wait — that's 11 slots. The new makeDeferred() is
    // 12 (UI stays, GBufferDebug appended last).
    const auto desc = ayt::render::RenderPipelineDesc::makeDeferred();
    // 12 slots pre-existing (Shadow..UI) + 1 GBufferDebug = 12
    // (deferred pipeline total is 12 slots with GBufferDebug).
    CHECK(desc.passes.size() == 12u);
}

// ─── H. PassExecContext trailing-default ABI lock ──────────────────

TEST_CASE(v1_pass_exec_context_gbufferdebug_fbo_defaults_invalid) {
    // Append-only trailing default: every existing 23-field
    // brace-init test site (Test_SSAO_A1, Test_B2_GBufferPass,
    // Test_F2_ForwardShadow, ...) keeps compiling without edits.
    // Verify the new field defaults to INVALID so the gating
    // contract is enforced at the type level.
    GBufferDebugV1Stubs stubs;
    PassExecContext ctx{
        stubs.adapter,
        stubs.pool,
        stubs.scene,
        stubs.meshes,
        stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        0u,
    };
    // Pre-fill gbufferDebugFbo is default-constructed ⇒ INVALID.
    CHECK(ctx.gbufferDebugFbo.idx == UINT16_MAX);
    // Other trailing-default fields unchanged (regression).
    CHECK(ctx.gbufferPass    == nullptr);
    CHECK(ctx.frameGraph     == nullptr);
}

TEST_SUITE_END
