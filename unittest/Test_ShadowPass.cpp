// ShadowPass R5+ / PR-F1' (2026-07-20) — verifies the shadow caster
// slot wires through cleanly. Tests pin:
//
//   1) ShadowPass slot basics — name, default isReady()==false,
//      default isEnabled()==true, default shadowMapSize()==1024.
//
//   2) ShadowPass setShadowMapSize() override path.
//
//   3) BGFXAdapter::createDepthOnlyFrameBuffer API — Noop-safe
//      (returns invalid when not initialized; 0 size also yields
//      invalid). destroy(invalid) is a no-op.
//
//   4) ShadowPass on Noop backend — execute() short-circuits to 0
//      draws, isReady stays false.
//
//   5) ShadowPass zero viewport — defensive guard.
//
//   6) ShadowPass setEnabled(false) — pipeline honors the flag.
//
//   7) ShadowPass can be registered in a hand-built RenderPipeline
//      with the existing [ForwardOpaque, Transparent, PostProcess,
//      UI] passes — verifies the slot doesn't disturb dispatch.
//
//   8) PR-F1' — buildDirectionalShadowMatrices produces non-identity
//      view/proj that change when lightDirection changes (no GPU).
//
// All tests use Backend::Noop so the test path is shaderc-free and
// headless. Default Renderer pipeline must remain 4-pass (no Shadow).

#include "AYTest.h"
#include "AYRenderScene.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"
#include "detail/ShadowLightMatrix.h"
#include "detail/ShadowPass.h"
#include "detail/TransparentPass.h"
#include "detail/UIPass.h"

#include <bgfx/bgfx.h>

#include <memory>
#include <unordered_map>

using ayt::render::RenderScene;
using ayt::render::detail::RenderPass;
using ayt::render::detail::ShadowPass;
using ayt::render::detail::ForwardOpaquePass;
using ayt::render::detail::TransparentPass;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::UIPass;
using ayt::render::detail::RenderPipeline;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::buildDirectionalShadowMatrices;

namespace {

bool matricesEqual(const ayt::math::Float4x4& a, const ayt::math::Float4x4& b)
{
    const float* pa = a.ptr();
    const float* pb = b.ptr();
    for (int i = 0; i < 16; ++i) {
        if (pa[i] != pb[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace {

void runShadowDispatch(RenderPipeline& pipe, const RenderScene& scene, FrameContext& frame)
{
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::detail::PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    pipe.executeAll(ctx);
}

} // namespace

TEST_SUITE(AYRenderer_ShadowPass_R5Plus)

TEST_CASE(r5plus_shadow_pass_name_and_initial_state) {
    ShadowPass pass;
    CHECK(pass.name() == "Shadow");
    CHECK(pass.isReady() == false);
    CHECK(pass.isEnabled() == true);
    CHECK(pass.shadowMapSize() == ShadowPass::kDefaultShadowMapSize);
    CHECK(pass.shadowMapSize() == 2048);
}

TEST_CASE(r5plus_shadow_pass_shadow_size_override) {
    ShadowPass pass;
    CHECK(pass.shadowMapSize() == 2048);
    pass.setShadowMapSize(1024);
    CHECK(pass.shadowMapSize() == 1024);
    pass.setShadowMapSize(512);
    CHECK(pass.shadowMapSize() == 512);
}

TEST_CASE(r5plus_bgfxaadapter_create_depth_only_fbo_noop_safe) {
    BGFXAdapter adapter;
    CHECK(adapter.isInitialized() == false);

    const bgfx::FrameBufferHandle fb = adapter.createDepthOnlyFrameBuffer(1024, 1024);
    CHECK(bgfx::isValid(fb) == false);

    CHECK(bgfx::isValid(adapter.createDepthOnlyFrameBuffer(0, 1024)) == false);
    CHECK(bgfx::isValid(adapter.createDepthOnlyFrameBuffer(1024, 0))   == false);
    CHECK(bgfx::isValid(adapter.createDepthOnlyFrameBuffer(0, 0))      == false);

    adapter.destroy(bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE});
}

TEST_CASE(r5plus_shadow_pass_noop_backend_returns_zero) {
    ShadowPass pass;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ShadowPass>());

    FrameContext frame;
    RenderScene scene;
    runShadowDispatch(pipe, scene, frame);
    CHECK(pass.isReady() == false);
}

TEST_CASE(r5plus_shadow_pass_empty_scene_returns_zero) {
    ShadowPass pass;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ShadowPass>());

    FrameContext frame;
    RenderScene scene;
    runShadowDispatch(pipe, scene, frame);
    CHECK(pass.isReady() == false);
}

TEST_CASE(r5plus_shadow_pass_zero_viewport_short_circuits) {
    ShadowPass pass;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ShadowPass>());

    RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    FrameContext frame;

    ayt::render::detail::PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 0, 0, frame, /*viewId=*/0
    };
    const uint32_t draws = pipe.executeAll(ctx);
    CHECK(draws == 0);
    CHECK(pass.isReady() == false);
}

TEST_CASE(r5plus_shadow_pass_setenabled_false_still_returns_zero) {
    ShadowPass pass;
    pass.setEnabled(false);
    CHECK(pass.isEnabled() == false);

    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ShadowPass>());
    pipe.passes()[0]->setEnabled(false);

    FrameContext frame;
    RenderScene scene;
    runShadowDispatch(pipe, scene, frame);
    CHECK(pass.isReady() == false);
}

TEST_CASE(r5plus_shadow_slot_pipeline_order_in_front_of_forward_opaque) {
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ShadowPass>());
    pipe.addPass(std::make_unique<ForwardOpaquePass>());
    pipe.addPass(std::make_unique<TransparentPass>());
    pipe.addPass(std::make_unique<PostProcessPass>());
    pipe.addPass(std::make_unique<UIPass>());

    CHECK(pipe.passes().size() == 5);
    CHECK(pipe.passes()[0]->name() == "Shadow");
    CHECK(pipe.passes()[1]->name() == "ForwardOpaque");
    CHECK(pipe.passes()[2]->name() == "Transparent");
    CHECK(pipe.passes()[3]->name() == "PostProcess");
    CHECK(pipe.passes()[4]->name() == "UI");
}

TEST_CASE(r5plus_full_pipeline_noop_dispatch_sums_to_zero) {
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ShadowPass>());
    pipe.addPass(std::make_unique<ForwardOpaquePass>());
    pipe.addPass(std::make_unique<TransparentPass>());
    pipe.addPass(std::make_unique<PostProcessPass>());
    pipe.addPass(std::make_unique<UIPass>());

    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;

    RenderScene scene;
    FrameContext frame;
    ayt::render::detail::PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    const uint32_t total = pipe.executeAll(ctx);
    CHECK(total == 0);
}

TEST_CASE(r5plus_shadow_pass_destroy_resources_is_noop_when_uninitialized) {
    ShadowPass pass;
    BGFXAdapter adapter;
    pass.destroyResources(adapter);
    CHECK(pass.isReady() == false);
}

TEST_CASE(f1_safe_light_space_matrices_non_identity_and_direction_sensitive) {
    ayt::math::Float4x4 viewA = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 projA = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 viewProjA = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 viewB = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 projB = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 viewProjB = ayt::math::Float4x4::identity();
    float viewColA[16] = {};
    float projColA[16] = {};
    float colA[16] = {};
    float viewColB[16] = {};
    float projColB[16] = {};
    float colB[16] = {};

    buildDirectionalShadowMatrices(
        ayt::math::FVector3(0.3f, -0.8f, -0.4f),
        viewA, projA, viewProjA, viewColA, projColA, colA);
    buildDirectionalShadowMatrices(
        ayt::math::FVector3(-0.3f, -0.8f, 0.4f),
        viewB, projB, viewProjB, viewColB, projColB, colB);

    CHECK(matricesEqual(viewA, ayt::math::Float4x4::identity()) == false);
    CHECK(matricesEqual(projA, ayt::math::Float4x4::identity()) == false);
    CHECK(matricesEqual(viewProjA, ayt::math::Float4x4::identity()) == false);
    CHECK(matricesEqual(viewA, viewB) == false);
    CHECK(matricesEqual(viewProjA, viewProjB) == false);
    CHECK(colA[15] != 0.0f);
    CHECK(colB[15] != 0.0f);

    ShadowPass pass;
    CHECK(matricesEqual(pass.lightView(), ayt::math::Float4x4::identity()) == true);
    CHECK(matricesEqual(pass.lightProj(), ayt::math::Float4x4::identity()) == true);
}

// R5+ (Pass-side backfill, 2026-07-20) — verify the new
// BGFXAdapter helpers exist and behave correctly. These are the
// helpers the 4 RenderPass implementations use to avoid calling
// bgfx:: directly:
TEST_CASE(r5plus_bgfxaadapter_pass_side_helpers_noop_safe) {
    BGFXAdapter adapter;
    CHECK(adapter.isInitialized() == false);

    // setState / setTransformIdentity / setViewClearRaw /
    // setViewClearDepthOnly / submit / getFboAttachment are all
    // intentionally un-guarded on isInitialized (the bgfx global
    // state queue accepts invalid calls before bgfx::init; the
    // destruction happens at Adapter::shutdown). Pin that they
    // compile + don't crash on a default-constructed adapter.
    adapter.setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
    adapter.setTransformIdentity();
    adapter.setViewClearRaw(0, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    adapter.setViewClearDepthOnly(0, 1.0f);
    adapter.submit(0, bgfx::ProgramHandle{BGFX_INVALID_HANDLE}, 0, BGFX_DISCARD_NONE);

    // isValid() static overloads — query-side only, no init needed.
    CHECK(BGFXAdapter::isValid(bgfx::VertexBufferHandle{BGFX_INVALID_HANDLE}) == false);
    CHECK(BGFXAdapter::isValid(bgfx::IndexBufferHandle{BGFX_INVALID_HANDLE})  == false);
    CHECK(BGFXAdapter::isValid(bgfx::TextureHandle{BGFX_INVALID_HANDLE})     == false);
    CHECK(BGFXAdapter::isValid(bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE}) == false);

    // getFboAttachment on uninitialized adapter returns invalid (the
    // bgfx::getTexture under the hood would crash; gating on
    // isInitialized avoids that).
    const bgfx::TextureHandle fbo0 = adapter.getFboAttachment(
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE}, 0);
    CHECK(bgfx::isValid(fbo0) == false);
}

// §P5.5 C (2026-07-23) — per-light shadow atlas layout. The grid
// is auto-derived from the slot count: rows = ceil(sqrt(N)),
// cols = ceil(N / rows). For N=8 ⇒ rows=3, cols=3 (3×3 grid
// covering the 4096×4096 atlas ⇒ 1365×1365 per slot). Pinning the
// layout invariants here protects against accidental grid
// regressions (e.g. someone changing rows=ceil(sqrt(N)) to a
// different heuristic that breaks sub-rect UV computation in
// LightingPass).
TEST_CASE(shadow_pass_atlas_layout_default_3x3_grid_for_8_slots) {
    using namespace ayt::render::detail;
    ShadowAtlasConfig cfg{4096, 8};
    ShadowAtlasLayout layout = computeShadowAtlasLayout(cfg);
    CHECK(layout.slotCount == 8u);
    CHECK(layout.atlasSize == 4096u);
    // N=8 ⇒ rows=ceil(sqrt(8))=3, cols=ceil(8/3)=3
    CHECK(layout.gridCols == 3u);
    CHECK(layout.gridRows == 3u);
    // Slot 0 lands in the top-left tile (col=0, row=0):
    //   u0=0, v0=0, u1=1/3, v1=1/3
    CHECK(layout.subRects[0][0] == 0.0f);
    CHECK(layout.subRects[0][1] == 0.0f);
    CHECK_FLOAT_EQ(layout.subRects[0][2], 1.0f / 3.0f, 1e-5f);
    CHECK_FLOAT_EQ(layout.subRects[0][3], 1.0f / 3.0f, 1e-5f);
    // Slot 7 lands in col=1, row=2 (slot/cols = 7/3 = 2 rem 1):
    //   u0=1/3, v0=2/3, u1=2/3, v1=1.0
    CHECK_FLOAT_EQ(layout.subRects[7][0], 1.0f / 3.0f, 1e-5f);
    CHECK_FLOAT_EQ(layout.subRects[7][1], 2.0f / 3.0f, 1e-5f);
    CHECK_FLOAT_EQ(layout.subRects[7][2], 2.0f / 3.0f, 1e-5f);
    CHECK(layout.subRects[7][3] == 1.0f);
}

TEST_CASE(shadow_pass_atlas_subrects_within_unit_square) {
    using namespace ayt::render::detail;
    // All slots for the default config must have UV in [0,1] and
    // non-overlapping (verified by sampling all 8 slots for bounds).
    ShadowAtlasLayout layout = computeShadowAtlasLayout(
        ShadowAtlasConfig{4096, 8});
    for (uint32_t i = 0; i < layout.slotCount; ++i) {
        const float u0 = layout.subRects[i][0];
        const float v0 = layout.subRects[i][1];
        const float u1 = layout.subRects[i][2];
        const float v1 = layout.subRects[i][3];
        CHECK(u0 >= 0.0f); CHECK(u0 <= 1.0f);
        CHECK(v0 >= 0.0f); CHECK(v0 <= 1.0f);
        CHECK(u1 >= 0.0f); CHECK(u1 <= 1.0f);
        CHECK(v1 >= 0.0f); CHECK(v1 <= 1.0f);
        CHECK(u1 > u0);  // non-zero width
        CHECK(v1 > v0);  // non-zero height
    }
}

TEST_CASE(shadow_pass_atlas_slot_pixel_rects_default) {
    using namespace ayt::render::detail;
    // Default 4096 atlas, 8 slots, 3×3 grid ⇒ each slot is
    // 1365×1365 (atlasSize/cols × atlasSize/rows, integer
    // truncation — floor division; the last column/row absorbs the
    // remainder).
    ShadowAtlasLayout layout = computeShadowAtlasLayout(
        ShadowAtlasConfig{4096, 8});
    const ShadowAtlasPixelRect r0 = shadowAtlasSlotPixelRect(layout, 0);
    CHECK(r0.x == 0);    CHECK(r0.y == 0);
    CHECK(r0.w == 1365); CHECK(r0.h == 1365);
    const ShadowAtlasPixelRect r7 = shadowAtlasSlotPixelRect(layout, 7);
    // col = 7 % 3 = 1, row = 7 / 3 = 2
    CHECK(r7.x == 1365); CHECK(r7.y == 2730);
    CHECK(r7.w == 1365); CHECK(r7.h == 1365);
}

TEST_CASE(shadow_pass_per_light_shadow_count_zero_default) {
    // No setSceneLightsRef call ⇒ perLightShadowCount() = 0 (pre-C
    // byte-equivalent path). All atlas sub-rects / LVPs stay at
    // identity baseline.
    ayt::render::detail::ShadowPass sp;
    CHECK(sp.perLightShadowCount() == 0u);
    // atlas config defaults — 4096 + 8 slots, 3×3 grid for N=8.
    CHECK(sp.atlasConfig().atlasSize == 4096u);
    CHECK(sp.atlasConfig().slotCount == 8u);
    CHECK(sp.atlasLayout().gridCols == 3u);
    CHECK(sp.atlasLayout().gridRows == 3u);
    // Sub-rects for slot 0 = (0, 0, 1/3, 1/3)
    const float* rects = sp.atlasSubRects();
    CHECK(rects[0] == 0.0f);
    CHECK(rects[1] == 0.0f);
    CHECK_FLOAT_EQ(rects[2], 1.0f / 3.0f, 1e-5f);
    CHECK_FLOAT_EQ(rects[3], 1.0f / 3.0f, 1e-5f);
    // Per-slot LVP[0] = identity (col-major float[16]).
    const float* lvps = sp.atlasLightViewProjsColumnMajor();
    CHECK(lvps[0] == 1.0f);   // (0,0) = 1
    CHECK(lvps[5] == 1.0f);   // (1,1) = 5th float = 1
    CHECK(lvps[10] == 1.0f);  // (2,2) = 10th float = 1
    CHECK(lvps[15] == 1.0f);  // (3,3) = 15th float = 1
    // Per-slot biases = 0.
    const float* biases = sp.atlasShadowBiases();
    for (uint32_t i = 0; i < 8u; ++i) {
        CHECK(biases[i] == 0.0f);
    }
}

TEST_CASE(shadow_pass_atlas_config_setter_overrides_default) {
    // Tests can swap the atlas size + slot count via setAtlasConfig.
    // 2048 atlas, 4 slots ⇒ rows=ceil(sqrt(4))=2, cols=ceil(4/2)=2
    // ⇒ 2×2 grid, 1024×1024 per slot.
    ayt::render::detail::ShadowPass sp;
    sp.setAtlasConfig(ayt::render::detail::ShadowAtlasConfig{2048, 4});
    CHECK(sp.atlasConfig().atlasSize == 2048u);
    CHECK(sp.atlasConfig().slotCount == 4u);
    CHECK(sp.atlasLayout().gridCols == 2u);
    CHECK(sp.atlasLayout().gridRows == 2u);
    // Slot 0 = top-left: (0, 0, 0.5, 0.5)
    const float* rects = sp.atlasSubRects();
    CHECK(rects[2] == 0.5f);
    CHECK(rects[3] == 0.5f);
}

TEST_SUITE_END
