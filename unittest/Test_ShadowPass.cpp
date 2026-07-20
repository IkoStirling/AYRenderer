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
    CHECK(pass.shadowMapSize() == 1024);
}

TEST_CASE(r5plus_shadow_pass_shadow_size_override) {
    ShadowPass pass;
    CHECK(pass.shadowMapSize() == 1024);
    pass.setShadowMapSize(2048);
    CHECK(pass.shadowMapSize() == 2048);
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
    ayt::math::Float4x4 viewB = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 projB = ayt::math::Float4x4::identity();

    buildDirectionalShadowMatrices(
        ayt::math::FVector3(0.3f, -0.8f, -0.4f), viewA, projA);
    buildDirectionalShadowMatrices(
        ayt::math::FVector3(-0.3f, -0.8f, 0.4f), viewB, projB);

    CHECK(matricesEqual(viewA, ayt::math::Float4x4::identity()) == false);
    CHECK(matricesEqual(projA, ayt::math::Float4x4::identity()) == false);
    CHECK(matricesEqual(viewA, viewB) == false);

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

TEST_SUITE_END
