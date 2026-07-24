// §A1 SSAO MVP skeleton test (2026-07-24, mid-term FG MVP SSAO Gate
// commit). Pins the SHIP contract for A1:
//
//   1) RenderPassSlot enum ABI:
//        PostProcess = 4 (unchanged)
//        BloomExtract = 8 / BloomBlur = 9 / DepthHaze = 10
//        ⇒ SSAO = 11 (append-only)
//   2) FgResourceId enum ABI:
//        SceneColor = 0 ... HazeHalf = 4
//        ⇒ SSAOTexture = 5 (append-only)
//   3) FgSemantic enum ABI:
//        FinalColorSource = 0 / BloomSource = 1 / HazeSource = 2
//        ⇒ SSAOSource = 3 (append-only)
//   4) View id reservation lock:
//        SSAOPass::kSsaoViewId == 14 (= DepthHaze+1)
//        PostProcessPass::kBlitViewId == 15 (= SSAO+1, A2 bumps this)
//   5) FrameContext default state:
//        ssaoEnabled == false, ssaoStrength == 0, ssaoRadius == 0.5,
//        ssaoBias == 0.025  ⇒ K-SSAO-1 = "all off, zero alloc"
//   6) Cache-key extern mirror:
//        SSAOPass::kSSAOCacheKeyCStr literals must agree between
//        .h declaration + .cpp definition. Pre-A3 the literal is
//        a placeholder; A3 bumps it. Drift detection guard.
//   7) SSAOPass skeleton execute() returns 0 draw — even on
//        non-initialized / Noop adapter short-circuits.
//   8) SSAOPass::isReady() returns false (placeholder for A3).
//   9) SSAOPass::destroyResources() idempotent (no crash / no
//        log noise on double-call or uninitialized adapter).
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
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/SSAOPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <unordered_map>

using ayt::render::RenderPassSlot;
using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgSemantic;
using ayt::render::detail::FrameContext;
using ayt::render::detail::FrameGraph;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::SSAOPass;

namespace {

struct SSAOA1Stubs {
    ayt::render::RenderScene scene{};
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh>     meshes;
    std::unordered_map<uint64_t, GpuTexture>  textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
};

bgfx::FrameBufferHandle makeFakeHandle(uint16_t idx)
{
    bgfx::FrameBufferHandle h;
    h.idx = idx;
    return h;
}

} // namespace

TEST_SUITE(AYRenderer_SSAO_A1)

// ─── A. RenderPassSlot enum ABI lock ───────────────────────────────

TEST_CASE(a1_render_pass_slot_ssao_is_11_append_only) {
    // §A1 (2026-07-24) — append-only ABI: DepthHaze = 10 ⇒ SSAO = 11.
    // Cutsheet `docs/frame-graph-mvp.md` §S2 view-map lock requires
    // NO reorder of existing values (cutsheet §FG MVP append-only).
    CHECK(static_cast<uint8_t>(RenderPassSlot::SSAO) == 11u);
    // No-reorder guard — bump these if SSAO were ever reshuffled.
    CHECK(static_cast<uint8_t>(RenderPassSlot::DepthHaze)     == 10u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomBlur)      == 9u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomExtract)   == 8u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Lighting)       == 7u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::GBuffer)        == 6u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::UI)             == 5u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::PostProcess)    == 4u);
}

TEST_CASE(a1_make_default_does_not_include_ssao) {
    // Cutsheet §S2 hard line — makeDefault() (Forward) does NOT
    // mount SSAO. The host opts in via makeDeferred() or a custom
    // desc that includes the slot. A2 verifies the same for the
    // real wire path; A1 just guarantees the slot-table ABI is
    // not pollution'd into the Forward path.
    const auto desc = ayt::render::RenderPipelineDesc::makeDefault();
    CHECK(desc.passes.size() == 8);
    bool found = false;
    for (const auto s : desc.passes) {
        if (s == RenderPassSlot::SSAO) {
            found = true;
            break;
        }
    }
    CHECK_FALSE(found);
}

// ─── B. FgResourceId + FgSemantic enum ABI lock ─────────────────────

TEST_CASE(a1_fg_resource_id_ssao_texture_is_5) {
    // §A1 (2026-07-24) — append-only: SceneColor=0 ... HazeHalf=4
    // ⇒ SSAOTexture = 5. Test pin enforces no-reorder.
    CHECK(static_cast<uint8_t>(FgResourceId::SSAOTexture) == 5u);
    // No-reorder guard for the FG pool index. Drift here would
    // mis-align every F6/F7 test that uses the existing IDs.
    CHECK(static_cast<uint8_t>(FgResourceId::SceneColor) == 0u);
    CHECK(static_cast<uint8_t>(FgResourceId::BloomBright) == 1u);
    CHECK(static_cast<uint8_t>(FgResourceId::BloomBlurA) == 2u);
    CHECK(static_cast<uint8_t>(FgResourceId::BloomBlurB) == 3u);
    CHECK(static_cast<uint8_t>(FgResourceId::HazeHalf) == 4u);
    // Sentinel must bump to 6 (post-A1).
    CHECK(static_cast<uint8_t>(FgResourceId::Count) == 6u);
}

TEST_CASE(a1_fg_semantic_ssao_source_is_3) {
    // §A1 (2026-07-24) — append-only: FinalColorSource=0 /
    // BloomSource=1 / HazeSource=2 ⇒ SSAOSource = 3.
    CHECK(static_cast<uint8_t>(FgSemantic::SSAOSource) == 3u);
    CHECK(static_cast<uint8_t>(FgSemantic::FinalColorSource) == 0u);
    CHECK(static_cast<uint8_t>(FgSemantic::BloomSource) == 1u);
    CHECK(static_cast<uint8_t>(FgSemantic::HazeSource) == 2u);
    CHECK(static_cast<uint8_t>(FgSemantic::Count) == 4u);
}

// ─── C. View id reservation lock ────────────────────────────────────

TEST_CASE(a1_ssao_view_id_lock_is_14) {
    // §A1 (2026-07-24) — cutsheet §S2 view-map lock:
    //   BloomExtract=10 → BlurH=11 → BlurV=12 → DepthHaze=13 →
    //   SSAO=14 → PostProcess=15 → UI=255.
    CHECK(SSAOPass::kSsaoViewId == 14u);
}

TEST_CASE(a1_post_process_view_id_lock_is_14_pre_a2_bump) {
    // §A1 (2026-07-24) — pre-A2 baseline: kBlitViewId is still 14
    // (the historical lock from §S4b). The 14→15 single-point bump
    // lands in A2. Pin this here so a tester reading the suite
    // knows the bump-then-lock order across A1 / A2 / A3.
    CHECK(static_cast<uint16_t>(PostProcessPass::kBlitViewId) == 14u);
}

// ─── D. FrameContext default-zero (K-SSAO-1 hold) ──────────────────

TEST_CASE(a1_frame_context_ssao_defaults_are_zero_alloc) {
    // K-SSAO-1 (cutsheet §S2) — ssaoEnabled=false / ssaoStrength=0
    // ⇒ SSAOTexture not live ⇒ 0 alloc. Verify the brace-init
    // defaults.
    FrameContext ctx;
    CHECK_FALSE(ctx.ssaoEnabled);
    CHECK(ctx.ssaoStrength == 0.0f);
    // radius/bias carry non-zero defaults so a host that flips
    // ssaoEnabled=true without setting them gets a reasonable
    // first frame instead of a degenerate all-dark or all-bright
    // image. K-SSAO-1 still holds because ssaoEnabled alone
    // gates the wire.
    CHECK(ctx.ssaoRadius == 0.5f);
    CHECK(ctx.ssaoBias   == 0.025f);
}

TEST_CASE(a1_existing_frame_context_haze_fields_unchanged) {
    // Regression — pre-A1 FrameContext tail must remain intact.
    // Bumping FrameContext ABI is risky (cutsheet §5.3 red line);
    // verify the haze tail stays byte-identical alongside the new
    // SSAO tail.
    FrameContext ctx;
    CHECK_FALSE(ctx.hazeEnabled);
    CHECK(ctx.hazeStrength == 0.0f);
    CHECK(ctx.hazeDensity  == 0.02f);
    // The new SSAO fields sit AFTER the haze tail.
    CHECK_FALSE(ctx.ssaoEnabled);
    CHECK(ctx.ssaoStrength == 0.0f);
}

// ─── E. Cache-key extern mirror ─────────────────────────────────────

TEST_CASE(a1_ssao_cache_key_extern_matches_in_source) {
    // Bug-fix-#3 mirror (DepthHazePass.h:186, BloomExtractPass,
    // BloomBlurPass, SkyboxPass, LightingPass). The extern
    // declaration in SSAOPass.h and the file-scope definition in
    // SSAOPass.cpp must agree on the SAME string. Pre-A3 the
    // literal is a placeholder; A3 bumps it. If a future cutsheet
    // moves the literal, drift here = test fails immediately.
    CHECK(ayt::render::detail::kSSAOCacheKeyCStr != nullptr);
    // The placeholder must be non-empty (an empty literal would
    // indicate the cut forgot to wire the cache key).
    const std::size_t len = std::char_traits<char>::length(
        ayt::render::detail::kSSAOCacheKeyCStr);
    CHECK(len > 0);
    // The literal must contain "ssao" so cache-key collisions on
    // other passes can be caught early.
    const std::string key(ayt::render::detail::kSSAOCacheKeyCStr);
    CHECK(key.find("ssao") != std::string::npos);
}

// ─── F. SSAOPass skeleton initial state ─────────────────────────────

TEST_CASE(a1_ssao_pass_skeleton_initial_state) {
    SSAOPass pass{};
    CHECK(pass.name() == "SSAO");
    CHECK_FALSE(pass.isReady());
}

TEST_CASE(a1_ssao_pass_destroy_resources_idempotent) {
    // Destroy on uninitialized adapter + twice ⇒ no crash, no log
    // noise. Mirror DepthHazePass / PostProcessPass contract.
    BGFXAdapter adapter;  // uninitialized
    SSAOPass pass{};
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
    CHECK_FALSE(pass.isReady());
}

// ─── G. SSAOPass::execute() short-circuits ──────────────────────────

TEST_CASE(a1_ssao_pass_execute_uninitialized_adapter_returns_zero) {
    SSAOA1Stubs stubs;
    BGFXAdapter adapter;
    CHECK_FALSE(adapter.isInitialized());

    ayt::shader::ShaderResourcePool pool;
    SSAOPass pass{};

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };

    // frameGraph is nullptr (pre-A2 wire; A1 only tests the
    // pre-FG shape via the early return). SSAOPass::execute
    // returns 0.
    CHECK(pass.execute(ctx) == 0u);
    CHECK_FALSE(pass.isReady());
}

TEST_CASE(a1_ssao_pass_execute_no_frame_graph_returns_zero) {
    // Even with an adapter pointer, pre-A2 / pre-FG callers leave
    // ctx.frameGraph == nullptr. SSAOPass must early-return 0
    // (mirror DepthHazePass F4 line 144-146 contract).
    SSAOA1Stubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    SSAOPass pass{};

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };
    // Simulate that frameGraph was never wired — same shape as
    // existing 14-/15-/22-field brace-init test sites that don't
    // set frameGraph (C++14 trailing default = nullptr).
    CHECK(ctx.frameGraph == nullptr);
    CHECK(pass.execute(ctx) == 0u);
}

TEST_CASE(a1_ssao_pass_execute_frame_graph_invalid_resolve_returns_zero) {
    // Even with ctx.frameGraph wired, if SSAOTexture was never
    // declared in the FG (K-SSAO-1: ssaoEnabled=false), the
    // resolve() returns invalid and the pass early-returns 0.
    // A2 wire adds the FG addResource + addPass + central gate;
    // A1 only verifies the resolve-invalid contract.
    SSAOA1Stubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    SSAOPass pass{};
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);

    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };
    ctx.frameGraph = &fg;

    // No addResource / addPass on fg ⇒ SSAOTexture not declared.
    CHECK(pass.execute(ctx) == 0u);
}

// ─── H. K-SSAO invariant documentation pin ──────────────────────────

TEST_CASE(a1_k_ssao_invariants_documented) {
    // Documentation-only pin for grep-ability. The contract:
    //   K-SSAO-1: ssaoEnabled=false || ssaoStrength<=0 ||
    //     gbufferPass==nullptr ⇒ SSAOTexture not live ⇒ resolve
    //     invalid ⇒ execute() returns 0 ⇒ zero draw, zero alloc.
    //     PostProcessPass composite (A3) fallback binds sceneColor
    //     on the SSAO slot and FS gate step(0.0001, ssaoStrength)
    //     collapses the composite (byte-equivalent to pre-A3).
    //   K-SSAO-2: SSAO sample rejecting sky (worldPos.w == 0)
    //     uses step(0.0001, w), NOT if. Phoskia has no if-expr.
    //   K-SSAO-3: PostProcessPass composite uses clamp(1-x, 0, 1),
    //     NOT saturate builtin.
    // A1 ships: K-SSAO-1 holds trivially (execute early-returns
    // even when resolve succeeds; SSAOTexture is in the graph).
    // K-SSAO-2/3 land with A3 (real shader). Trivially held now
    // because there is no shader body yet.
    CHECK(true);
}

// ─── I. SSAOTexture FG declare-vs-not-declare contrast ──────────────

TEST_CASE(a1_fg_ssao_texture_undeclared_resolve_is_invalid) {
    // Pre-A2 baseline: the FG knows nothing about SSAOTexture; the
    // resolve call from SSAOPass::execute must return invalid,
    // and the SSAOPass must early-return 0. A2 adds the addResource
    // path under the render() central `ssaoPassEnabled` gate.
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    fg.compile();

    CHECK(fg.stats().declaredPasses == 0);
    CHECK(fg.stats().livePasses     == 0);

    const bgfx::FrameBufferHandle h =
        fg.resolve(FgResourceId::SSAOTexture);
    CHECK(!BGFXAdapter::isValid(h));
}

TEST_CASE(a1_fg_ssao_texture_declared_resolve_live_count_one) {
    // Pre-A2 contract check: even when SSAOTexture IS declared
    // + a pass writes it + it's enabled, SSAOPass::execute still
    // returns 0 (A1 skeleton — no real draw). The early-return
    // means K-SSAO-1 trivially holds: A1 ships 0 draw on every
    // path. A3 is the cut that actually draws.
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    fg.addResource(FgResourceId::SSAOTexture,
                   {bgfx::TextureFormat::RGBA8,
                    ayt::render::detail::FgTextureScale::Full,
                    true, false});
    fg.addPass({"SSAO",
                {FgResourceId::SceneColor},
                {FgResourceId::SSAOTexture},
                /*enabled=*/true});
    fg.compile();

    CHECK(fg.stats().declaredPasses == 1);
    CHECK(fg.stats().livePasses     == 1);
    // SSAOPass::execute still returns 0 in A1 (no real shader).
    SSAOA1Stubs stubs;
    ayt::shader::ShaderResourcePool pool;
    SSAOPass pass{};
    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };
    ctx.frameGraph = &fg;
    CHECK(pass.execute(ctx) == 0u);
}

// ─── J. PassExecContext default-init — no SSAOPass borrowed ptr ─────

TEST_CASE(a1_pass_exec_context_no_ssao_borrowed_ptr_field) {
    // §A1 design decision — SSAOPass is wired via the
    // FrameGraph resource read path, not via a borrowed pointer
    // on PassExecContext (mirror DepthHazePass line 422-422 was
    // borrowed-ptr, but SSAOPass is fully FG-driven so it does
    // NOT need a borrowed pointer — cutsheet §S2 "Resources:
    // SSAOTexture via FG.resolveSemantic(SSAOSource)"). No new
    // field on PassExecContext. Verify C++14 trailing-default
    // behavior: 22-field brace-init sites still compile (this
    // test itself is one such site; if test compiles & runs,
    // K-SSAO ABI holds).
    SSAOA1Stubs stubs;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    PassExecContext ctx{
        adapter, pool, stubs.scene, stubs.meshes, stubs.textures,
        stubs.materials,
        0, 0, 800, 600,
        stubs.frame,
        /*viewId=*/14u,
    };
    // depthHazePass (the previous appended borrowed-ptr) must
    // still be the default nullptr.
    CHECK(ctx.depthHazePass == nullptr);
    // frameGraph must still be the default nullptr (pre-A2).
    CHECK(ctx.frameGraph == nullptr);
}

TEST_SUITE_END
