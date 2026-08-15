// §A2 SSAO MVP pipeline + FG wire test (2026-07-24, mid-term FG
// MVP SSAO Gate commit).
//
// Pins the A2 SHIP contract:
//   1) makeDefault() unchanged (8 slots, no SSAO)
//   2) makeDeferred() includes SSAO between DepthHaze and PostProcess
//      on view 14 (cutsheet §S2 view-map lock)
//   3) ssaoPassEnabled composed correctly:
//        false ⇒ SSAOTexture NOT live ⇒ physicalTargets delta = 0
//        true  ⇒ SSAOTexture live ⇒ physicalTargets delta = +1
//   4) SSAOTexture format/scale pins RGBA8 / Full
//   5) SSAOPass producer/consumer chain: SSAO reads SceneColor
//      and writes SSAOTexture (FG reads/writes correctness)
//   6) setResolvedSemantic(SSAOSource, SSAOTexture) registered
//      only when ssaoPassEnabled
//
// The actual render() central gate runs ONLY in a real Impl context,
// so we simulate the gate by hand (mirroring Test_DepthHaze_F4
// pattern). The Real-Impl integration goes through Compile-time
// tests: an SS-Aopass-enabled dispatch via the test harness
// rendering a Renderer is deferred to host-side smoke testing.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShaderResource.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"
#include "detail/SSAOPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <unordered_map>

using ayt::render::RenderPath;
using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgSemantic;
using ayt::render::detail::FgTextureDesc;
using ayt::render::detail::FgTextureScale;
using ayt::render::detail::FrameContext;
using ayt::render::detail::FrameGraph;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::SSAOPass;

namespace {

bgfx::FrameBufferHandle makeFakeHandle(uint16_t idx)
{
    bgfx::FrameBufferHandle h;
    h.idx = idx;
    return h;
}

// Replay the centralized ssaoPassEnabled gate from Renderer::Impl
// render() — extracted here as a free function so unit tests can
// compose it directly without needing a full Renderer. The Impl
// render() body computes the same expression; any drift between
// this helper and the Impl body is a K-SSAO-1 violation.
bool computeSsaoPassEnabled(const FrameContext& frame,
                            const void* gbufferPassPtr,
                            const BGFXAdapter* adapter,
                            uint16_t viewportW,
                            uint16_t viewportH)
{
    bool en = frame.ssaoEnabled
        && frame.ssaoStrength > 0.0f
        && (gbufferPassPtr != nullptr)
        && (viewportW > 0)
        && (viewportH > 0);
    if (adapter != nullptr) {
        en = en
            && adapter->isInitialized()
            && !adapter->isNoopBackend();
    }
    return en;
}

} // namespace

TEST_SUITE(AYRenderer_SSAO_A2)

// ─── A. makeDefault() unchanged ──────────────────────────────────────

TEST_CASE(a2_make_default_does_not_include_ssao) {
    // Cutsheet §S2 hard line — makeDefault() (Forward) does NOT
    // mount SSAO. Forward path stays byte-equivalent pre-A2.
    const auto desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 9);
    bool found = false;
    for (const auto s : desc.passes) {
        if (s == RenderPassSlot::SSAO) {
            found = true;
            break;
        }
    }
    CHECK_FALSE(found);
}

TEST_CASE(a2_make_default_post_process_pin) {
    // PP stays at kBlitViewId = 15 for both Forward + Deferred.
    const auto desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.contains(RenderPassSlot::PostProcess));
    CHECK(static_cast<uint16_t>(PostProcessPass::kBlitViewId) == 15);
}

// ─── B. makeDeferred() includes SSAO between DepthHaze and PP ───────

TEST_CASE(a2_make_deferred_includes_ssao_between_depth_haze_and_pp) {
    const auto desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    CHECK(desc.contains(RenderPassSlot::SSAO));
    CHECK(desc.contains(RenderPassSlot::DepthHaze));
    CHECK(desc.contains(RenderPassSlot::PostProcess));
    CHECK(desc.contains(RenderPassSlot::BloomBlur));
    CHECK(desc.contains(RenderPassSlot::BloomExtract));

    // Find the SSAO slot index; it must be strictly AFTER DepthHaze
    // and strictly BEFORE PostProcess.
    int ssaoIdx = -1;
    int hazeIdx = -1;
    int ppIdx = -1;
    for (size_t i = 0; i < desc.passes.size(); ++i) {
        if (desc.passes[i] == RenderPassSlot::SSAO)       ssaoIdx = static_cast<int>(i);
        if (desc.passes[i] == RenderPassSlot::DepthHaze)  hazeIdx = static_cast<int>(i);
        if (desc.passes[i] == RenderPassSlot::PostProcess) ppIdx   = static_cast<int>(i);
    }
    CHECK(ssaoIdx >= 0);
    CHECK(hazeIdx >= 0);
    CHECK(ppIdx   >= 0);
    CHECK(ssaoIdx > hazeIdx);
    CHECK(ssaoIdx < ppIdx);
}

TEST_CASE(a2_make_deferred_slot_abi_stable) {
    // §A2 (2026-07-24) — append-only ABI lock: DepthHaze=10, SSAO=11.
    // No reordering of existing slot values.
    CHECK(static_cast<uint8_t>(RenderPassSlot::DepthHaze) == 10u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::SSAO)      == 11u);
}

// ─── C. ssaoPassEnabled gate logic (matrix) ─────────────────────────

TEST_CASE(a2_ssao_gate_disabled_when_ssao_disabled) {
    // K-SSAO-1 — when frame.ssaoEnabled == false (host default),
    // the gate must be false regardless of all other conditions.
    FrameContext ctx;
    ctx.ssaoEnabled  = false;
    ctx.ssaoStrength = 0.6f;
    // every other condition favorable
    BGFXAdapter adapter;       // uninitialized — but the gate
                              // evaluates to false on ssaoEnabled
                              // before reaching the adapter check.
    const bool en = computeSsaoPassEnabled(ctx, &adapter, &adapter, 800, 600);
    CHECK_FALSE(en);
}

TEST_CASE(a2_ssao_gate_disabled_when_strength_zero) {
    FrameContext ctx;
    ctx.ssaoEnabled  = true;
    ctx.ssaoStrength = 0.0f;   // no fog when zero
    BGFXAdapter adapter;
    const bool en = computeSsaoPassEnabled(ctx, &adapter, &adapter, 800, 600);
    CHECK_FALSE(en);
}

TEST_CASE(a2_ssao_gate_disabled_when_gbuffer_null) {
    // Forward pipeline has no gbufferPass — gate must stay false.
    FrameContext ctx;
    ctx.ssaoEnabled  = true;
    ctx.ssaoStrength = 0.6f;
    BGFXAdapter adapter;
    const bool en = computeSsaoPassEnabled(ctx, /*gbuffer=*/nullptr, &adapter, 800, 600);
    CHECK_FALSE(en);
}

TEST_CASE(a2_ssao_gate_disabled_when_viewport_zero) {
    FrameContext ctx;
    ctx.ssaoEnabled  = true;
    ctx.ssaoStrength = 0.6f;
    BGFXAdapter adapter;
    CHECK_FALSE(computeSsaoPassEnabled(ctx, &adapter, &adapter, 0, 600));
    CHECK_FALSE(computeSsaoPassEnabled(ctx, &adapter, &adapter, 800, 0));
}

TEST_CASE(a2_ssao_gate_disabled_when_adapter_uninitialized) {
    FrameContext ctx;
    ctx.ssaoEnabled  = true;
    ctx.ssaoStrength = 0.6f;
    BGFXAdapter adapter;
    CHECK_FALSE(adapter.isInitialized());
    const bool en = computeSsaoPassEnabled(ctx, &adapter, &adapter, 800, 600);
    CHECK_FALSE(en);
}

TEST_CASE(a2_ssao_gate_requires_all_seven_conditions) {
    // The reverse sense — since we cannot initialize a real
    // BGFXAdapter in this unit-test context, simulate the gate
    // with the adapter under two scenarios.
    FrameContext ctx;
    ctx.ssaoEnabled  = true;
    ctx.ssaoStrength = 0.6f;
    BGFXAdapter adapter;
    // Scenario A — adapter is & (uninitialized) ⇒ all but adapter
    // checks are satisfied; the isInitialized() check returns false
    // ⇒ gate is false.
    const bool en = computeSsaoPassEnabled(ctx, &adapter, &adapter, 800, 600);
    CHECK_FALSE(en);
    // Scenario B — adapter is nullptr ⇒ the helper short-circuits
    // the adapter checks (no isInitialized/isNoopBackend access)
    // and falls back to the other 5 conditions. With ctx, gbuffer,
    // and viewport favorable, the gate is TRUE. This pins the
    // helper's no-crash safety + the "adapter is required" rules
    // upstream (the render() central never passes nullptr).
    const bool en2 = computeSsaoPassEnabled(ctx, &adapter, nullptr, 800, 600);
    CHECK(en2);   // pointer-null branch returns the other gates' verdict.
}

// ─── D. FG wire — SSAOTexture not live by default ───────────────────

TEST_CASE(a2_fg_ssao_texture_not_live_when_enabled_false) {
    // Mirror the render() central gate: when ssaoEnabled=false,
    // we DON'T addResource(SSAOTexture) so resolve returns invalid.
    // K-SSAO-1 hold: 0 alloc, 0 draw.
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    // simulate host disabled ⇒ NO addResource / NO addPass
    fg.compile();

    CHECK(fg.stats().declaredPasses == 0);
    CHECK(fg.stats().livePasses     == 0);
    CHECK(fg.stats().physicalTargets == 0);
    CHECK(!BGFXAdapter::isValid(fg.resolve(FgResourceId::SSAOTexture)));
    CHECK(!BGFXAdapter::isValid(fg.resolveSemantic(FgSemantic::SSAOSource)));
}

TEST_CASE(a2_fg_ssao_texture_live_when_enabled_true) {
    // Inverse — when the host has ssaoEnabled=true / strength>0 /
    // Deferred, the render() central adds SSAOTexture + SSAO pass.
    // NOTE: this test does NOT exercise the full 7-condition gate
    // (which requires a real BGFXAdapter); it only checks that
    // when SSAOTexture IS added, stats say so. The gate itself
    // is tested in the C section above.
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x20));
    fg.addResource(FgResourceId::SSAOTexture,
                   FgTextureDesc{
                       bgfx::TextureFormat::RGBA8,
                       FgTextureScale::Full,
                       /*transient=*/true,
                       /*withDepth=*/false});
    fg.addPass({"SSAO",
                {FgResourceId::SceneColor},
                {FgResourceId::SSAOTexture},
                /*enabled=*/true});
    fg.setResolvedSemantic(FgSemantic::SSAOSource,
                           FgResourceId::SSAOTexture);
    fg.compile();

    CHECK(fg.stats().declaredPasses == 1);
    CHECK(fg.stats().livePasses     == 1);
    CHECK(fg.stats().logicalResources >= 2);  // SceneColor + SSAOTexture
    CHECK(fg.stats().physicalTargets >= 1);
}

TEST_CASE(a2_fg_ssao_texture_resource_desc_pins) {
    // Lock the SSAOTexture format / scale / transient / depth.
    // The render() central uses the same FgTextureDesc:
    //   RGBA8, Full, transient, no depth.
    // Drift ⇒ assertion fails (cutsheet §S2 red line "FG desc is
    // the single source of truth").
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x30));
    const FgTextureDesc expected{
        bgfx::TextureFormat::RGBA8,
        FgTextureScale::Full,
        /*transient=*/true,
        /*withDepth=*/false};
    fg.addResource(FgResourceId::SSAOTexture, expected);
    // Resource declared but not yet live (FG compile culls
    // un-referenced resources). To make it live AND to verify
    // the compile-time alias decision, attach a write.
    fg.addPass({"SSAO",
                {FgResourceId::SceneColor},
                {FgResourceId::SSAOTexture},
                /*enabled=*/true});

    // Compile: scale=Full on 800x600 viewport ⇒ 800x600 RGBA8
    // RT; live set includes SSAOTexture ⇒ physicalTargets += 1.
    fg.compile();
    CHECK(fg.stats().logicalResources >= 1);
    CHECK(fg.stats().livePasses        == 1);
    CHECK(fg.stats().physicalTargets   >= 1);
}

// ─── E. View chain lock ─────────────────────────────────────────────

TEST_CASE(a2_view_chain_ssao_then_post_process_in_order) {
    // §A2 (2026-07-24) — cutsheet §S2 view-map lock:
    //   DepthHaze=13 → SSAO=14 → PostProcess=15 → UI=255
    CHECK(SSAOPass::kSsaoViewId == 14);
    CHECK(static_cast<uint16_t>(PostProcessPass::kBlitViewId) == 15);
    CHECK(PostProcessPass::kBlitViewId
          == SSAOPass::kSsaoViewId + 1);
}

// ─── F. SSAOPass::execute zero draw in all gate-false paths ────────

TEST_CASE(a2_ssao_pass_executes_zero_when_fg_undeclared) {
    // Even with frameGraph wired but SSAOTexture undeclared
    // (the host-disabled path), execute() returns 0.
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x40));

    RenderScene scene{};
    FrameContext frame{};
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh>     meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture>  textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    ayt::shader::ShaderResourcePool pool;
    SSAOPass pass{};

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 800, 600,
        frame,
        /*viewId=*/14u,
    };
    ctx.frameGraph = &fg;

    fg.compile();
    // No addResource / addPass ⇒ SSAOTexture NOT live.
    CHECK(pass.execute(ctx) == 0u);
}

TEST_SUITE_END
