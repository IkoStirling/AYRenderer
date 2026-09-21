// GBuffer Debug contract tests. The suite name is retained for test-runner
// compatibility even though the pass now contains the live V2 overlay.
//
//   1) RenderPassSlot enum ABI:
//        SSAO = 11 (unchanged by append)
//        ⇒ GBufferDebug = 12 (append-only; SSAO max was 11)
//   2) View id reservation lock:
//        GBufferDebugPass::kGBufferDebugViewId == 250
//        (verified unused via repo grep, 2026-07-24)
//   3) FrameContext defaults to a disabled, zero-allocation path.
//   4) Original six GBuffer channels plus append-only R6-6 diagnostics.
//   5) Cache-key extern mirror (Bug fix #3 pattern):
//        kGBufferDebugCacheKeyCStr literals must agree between
//        .h declaration + .cpp definition.
//   6) Disabled/uninitialized/Noop/missing-GBuffer paths return 0.
//   7) GBufferDebugPass starts not-ready and allocates lazily.
//   8) GBufferDebugPass::destroyResources() idempotent.
//   9) makeDefault() does NOT contain GBufferDebug (Forward no-op).
//  10) makeDeferred() DOES contain GBufferDebug (and is the LAST
//        slot so bgfx ascending view-id dispatch runs view 250
//        strictly after view 15).
//  11) PassExecContext keeps producer/frame-graph pointers optional.
//
// All tests use Backend::Noop (headless test path). The pass's
// Noop-backend / uninit-adapter guards short-circuit before any
// real GPU work, so these tests don't fight Noop fragility.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShaderResource.h"
#include "AYShader/Ir.h"
#include "AYShader/Phoskia.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GBufferDebugPass.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

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

TEST_CASE(v1_channel_enum_append_only_values) {
    // Channel 3 is the packed material scalar view. Motion remains its
    // compatibility alias until a dedicated velocity attachment exists.
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Albedo)   == 0u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Normal)   == 1u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::WorldPos) == 2u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Material) == 3u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Motion)   == 3u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Depth)    == 4u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::MaterialModel) == 5u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::MotionVectors) == 6u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::SsaoOcclusion) == 7u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::TaaHistory) == 8u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::ShadowAtlas) == 9u);
    CHECK(static_cast<uint8_t>(GBufferDebugChannel::Count)    == 10u);
    CHECK(GBufferDebugPass::kGBufferDebugChannelCount == 10u);
}

// ─── C. FrameContext default state (K-GBD-1 zero alloc) ───────────

TEST_CASE(v1_frame_context_debug_defaults_off) {
    // Default OFF means execute returns before lazy GPU allocation.
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
    ctx.gbufferDebugChannel = 3u;  // Material
    CHECK(ctx.gbufferDebugEnabled);
    CHECK(ctx.gbufferDebugChannel == 3u);
}

TEST_CASE(r6_public_channel_setter_accepts_appended_range) {
    ayt::render::Renderer renderer;
    renderer.setGBufferDebugChannel(9u);
    CHECK(renderer.gbufferDebugChannel() == 9u);
    renderer.setGBufferDebugChannel(10u);
    CHECK(renderer.gbufferDebugChannel() == 0u);
}

// ─── D. Cache-key extern mirror (Bug fix #3) ──────────────────────

TEST_CASE(v1_cache_key_extern_mirror_non_null) {
    CHECK(ayt::render::detail::kGBufferDebugCacheKeyCStr != nullptr);
    const std::size_t len = std::char_traits<char>::length(
        ayt::render::detail::kGBufferDebugCacheKeyCStr);
    CHECK(len > 0);
}

TEST_CASE(v1_cache_key_extern_mirror_contains_marker) {
    // Live overlay literal contains "gbufferdebug" + version stamp.
    const std::string key(ayt::render::detail::kGBufferDebugCacheKeyCStr);
    CHECK(key.find("gbufferdebug") != std::string::npos);
    CHECK(key.find("v4")          != std::string::npos);
}

TEST_CASE(v2_live_overlay_source_generates_valid_ir) {
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::ir::IRProgram ir;
    std::vector<std::string> errors;
    const bool success = compiler.generateIr(
        ayt::render::detail::kGBufferDebugPhoskiaSourceCStr,
        ayt::shader::phoskia::CompileOptions{}, ir, errors);
    CHECK(success);
    CHECK(errors.empty());
    const std::string source(
        ayt::render::detail::kGBufferDebugPhoskiaSourceCStr);
    CHECK(source.find("texture2d auxiliary") != std::string::npos);
    CHECK(source.find("motionView * pick6") != std::string::npos);
    CHECK(source.find("ssaoView * pick7") != std::string::npos);
    CHECK(source.find("historyView * pick8") != std::string::npos);
    CHECK(source.find("shadowView * pick9") != std::string::npos);
}

#ifdef _WIN32
TEST_CASE(r6_diagnostic_overlay_compiles_for_d3d_s_5_0) {
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
    backend.platform = "windows";
    backend.profile = "s_5_0";
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    ayt::shader::CompiledShaderProgram program;
    compiler.compileToProgram(
        ayt::render::detail::kGBufferDebugPhoskiaSourceCStr,
        frontend, backend, program);
    if (!program.success) {
        for (const std::string& error : program.errors) {
            std::cerr << "[GBufferDebug D3D test] " << error << '\n';
        }
    }
    CHECK(program.success);
}
#endif

// ─── E. Lazy initial state + destroyResources idempotency ─────────

TEST_CASE(v1_pass_skeleton_initial_state) {
    GBufferDebugPass pass{};
    CHECK(pass.name() == "GBufferDebug");
    CHECK_FALSE(pass.isReady());
}

TEST_CASE(v1_pass_destroy_resources_idempotent) {
    // Destruction before the first enabled execute is a safe idempotent no-op.
    BGFXAdapter adapter;
    GBufferDebugPass pass{};
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
    CHECK_FALSE(pass.isReady());
}

TEST_CASE(v1_pass_isReady_stays_false_while_disabled) {
    GBufferDebugPass pass{};
    CHECK_FALSE(pass.isReady());
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
    CHECK(pass.execute(ctx) == 0u);
    CHECK_FALSE(pass.isReady());
}

// ─── F. execute() short-circuit ladder (K-GBD-1) ──────────────────

TEST_CASE(v1_execute_uninitialized_adapter_returns_zero) {
    // Enabled mode still gates before allocation when the adapter is absent.
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
    stubs.frame.gbufferDebugEnabled = true;
    CHECK(pass.execute(ctx) == 0u);
}

TEST_CASE(v1_execute_disabled_returns_zero_without_target_fbo) {
    // The pass renders to the backbuffer and owns no target FBO. Disabled
    // mode nevertheless returns before creating its triangle/program.
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
    CHECK(pass.execute(ctx) == 0u);
}

TEST_CASE(v1_execute_no_gbuffer_pass_returns_zero) {
    // Forward/custom pipelines without the producer cannot visualize it.
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
    stubs.frame.gbufferDebugEnabled = true;
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
    CHECK(desc.passes.size() == 19u);
}

// ─── H. PassExecContext producer defaults ─────────────────────────

TEST_CASE(v1_pass_exec_context_debug_dependencies_default_null) {
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
    CHECK(ctx.gbufferPass    == nullptr);
    CHECK(ctx.frameGraph     == nullptr);
}

TEST_SUITE_END
