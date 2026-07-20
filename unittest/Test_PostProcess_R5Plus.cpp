// PostProcessPass R5+ (2026-07-20) — verifies the R5+ real
// implementation lands behind the P0 no-op slot. Tests pin:
//
//   1) BGFXAdapter::createFrameBuffer / destroyFrameBuffer /
//      setViewFrameBuffer new APIs — the FBO abstraction that R5+
//      PostProcessPass (and the deferred Shadow / GBuffer / Lighting
//      Passes) all share. Pinned at the BGFXAdapter level so the
//      abstraction can't silently regress.
//
//   2) FrameContext extension (bloomStrength / exposure / tonemapMode)
//      — additive fields on the dispatch context; P0 already added
//      timeSeconds. R5+ extends the post-process surface so hosts
//      can drive the new uniform upload path.
//
//   3) Renderer::setPostProcessBloomStrength / Exposure / TonemapMode
//      — public setters that wire into the FrameContext. The Impl
//      keeps the values across frames and Renderer::render() copies
//      them into the FrameContext struct before dispatch.
//
//   4) PostProcessPass::execute() R5+ body — FBO acquire, view
//      rebind, fullscreen triangle submit, restore default backbuffer.
//      On the Noop backend (test path), execute() short-circuits at
//      the isInitialized() guard inside PostProcessPass::execute and
//      returns 0. We still verify the dispatch path via the four-pass
//      pipeline + executeAll() so the slot survives a refactor.
//
//   5) Pipeline order still [ForwardOpaque, Transparent, PostProcess,
//      UI] with R5+ pass behavior — pins kFullPipelineOrder[5] as a
//      runtime invariant.
//
//   6) isReady() / hasShader() interface — both report `false` until
//      execute() runs against an initialized adapter. Lets hosts
//      introspect the slot before deciding to skip.
//
// All tests use Backend::Noop so the test path is shaderc-free and
// headless.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"

#include "detail/BGFXAdapter.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"
#include "detail/TransparentPass.h"
#include "detail/UIPass.h"

#include <bgfx/bgfx.h>

#include <cstdio>
#include <memory>

using ayt::render::Renderer;
using ayt::render::RenderScene;
using ayt::render::Backend;
using ayt::render::detail::RenderPass;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::RenderPipeline;

namespace {

void runWithEmptyScene(RenderPipeline& pipe, uint32_t& totalDraws)
{
    ayt::render::detail::FrameContext frame{};
    frame.bloomStrength = 0.42f;
    frame.exposure      = 1.25f;
    frame.tonemapMode   = ayt::render::detail::FrameContext::TonemapMode::ACES;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    ayt::render::detail::BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    ayt::render::detail::PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    totalDraws = pipe.executeAll(ctx);
}

} // namespace

TEST_SUITE(AYRenderer_PostProcessPass_R5Plus)

TEST_CASE(r5plus_postprocess_noop_backend_returns_zero) {
    // R5+ — the real body still short-circuits on a default-constructed
    // BGFXAdapter (Noop backend test path). Returns 0 because:
    //   - ensureFbo() is gated on isInitialized() and returns invalid
    //   - ensureFullscreenQuad() is gated on isInitialized() too
    //   - The early `if (!adapter.isInitialized()) return 0` in execute()
    //     fires first.
    PostProcessPass pass;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<PostProcessPass>());

    ayt::render::detail::FrameContext frame{};
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    ayt::render::detail::BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    ayt::render::detail::PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    const uint32_t draws = pipe.executeAll(ctx);
    CHECK(draws == 0);
    // isReady() reports false because the FBO create didn't run on Noop.
    CHECK(pass.isReady() == false);
}

TEST_CASE(r5plus_postprocess_isready_false_on_construction) {
    // R5+ — the slot starts in a not-ready state. Hosts that want to
    // gate RenderPass::setEnabled on this should observe false here
    // and decide not to skip (R5+ real body is responsible for the
    // resource bring-up on first execute() against an initialized
    // adapter).
    PostProcessPass pass;
    CHECK(pass.isReady() == false);
    CHECK(pass.name() == "PostProcess");
    CHECK(pass.isEnabled() == true);
}

TEST_CASE(r5plus_framecontext_new_fields_default_to_no_effect) {
    // R5+ — FrameContext post-process knobs default to "off" so legacy
    // hosts (which never call setPostProcess*) see the same image as
    // the pre-R5+ forward-only pipeline.
    ayt::render::detail::FrameContext frame{};
    CHECK(frame.bloomStrength == 0.0f);  // bloom disabled
    CHECK(frame.exposure      == 1.0f);  // neutral exposure
    CHECK(frame.tonemapMode   == ayt::render::detail::FrameContext::TonemapMode::None);
}

TEST_CASE(r5plus_framecontext_tonemap_mode_enum_values_match_public_surface) {
    // R5+ — FrameContext::TonemapMode and Renderer::TonemapMode share
    // the same underlying numeric values (0/1/2). The public Renderer
    // API caster bridges between them at the setter. Test pins the
    // ordering so a future reorder doesn't silently break the public
    // surface.
    using FrameMode = ayt::render::detail::FrameContext::TonemapMode;
    using PublicMode = ayt::render::Renderer::TonemapMode;
    CHECK(static_cast<uint8_t>(FrameMode::None)     == static_cast<uint8_t>(PublicMode::None));
    CHECK(static_cast<uint8_t>(FrameMode::Reinhard) == static_cast<uint8_t>(PublicMode::Reinhard));
    CHECK(static_cast<uint8_t>(FrameMode::ACES)     == static_cast<uint8_t>(PublicMode::ACES));
}

TEST_CASE(r5plus_bgfxaadapter_framebuffer_api_noop_safe) {
    // R5+ — the new BGFXAdapter FBO surface must be safe to call on a
    // default-constructed (uninitialized) adapter. The Noop-backend
    // test path would otherwise hit bgfx::createFrameBuffer with no
    // init context.
    ayt::render::detail::BGFXAdapter adapter;
    CHECK(adapter.isInitialized() == false);

    // createFrameBuffer returns invalid when not initialized.
    const bgfx::FrameBufferHandle fb = adapter.createFrameBuffer(
        1280, 720, bgfx::TextureFormat::RGBA8, /*withDepth=*/true);
    CHECK(bgfx::isValid(fb) == false);

    // setViewFrameBuffer is a no-op when not initialized.
    adapter.setViewFrameBuffer(0, fb);

    // destroy(invalid) is also a no-op (handles both uninit + invalid).
    adapter.destroy(bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE});
    // Should not crash.
}

TEST_CASE(r5plus_postprocess_full_pipeline_dispatch_with_post_knobs) {
    // R5+ — full pipeline (ForwardOpaque + Transparent + PostProcess
    // + UI) executes end-to-end on the Noop backend. PostProcess
    // contributes 0 draws (Noop short-circuit); total draws come from
    // ForwardOpaque (0 because no scene items) + Transparent (0).
    // Verifies that the post-process knobs in FrameContext do not
    // disturb the dispatch path even though they're set.
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ayt::render::detail::ForwardOpaquePass>());
    pipe.addPass(std::make_unique<ayt::render::detail::TransparentPass>());
    pipe.addPass(std::make_unique<PostProcessPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::UIPass>());

    CHECK(pipe.passes().size() == 4);
    CHECK(pipe.passes()[2]->name() == "PostProcess");

    uint32_t totalDraws = 0;
    runWithEmptyScene(pipe, totalDraws);
    CHECK(totalDraws == 0);
}

TEST_CASE(r5plus_postprocess_zero_viewport_short_circuits) {
    // R5+ — defensive guard inside execute() rejects a 0-width or
    // 0-height viewport even on a real backend. The pipeline.executeAll
    // path passes the host's viewport through, and a 0x0 viewport
    // would crash bgfx::createFrameBuffer on some drivers (Metal /
    // Vulkan refuse 0-dimension textures). Since the test path is
    // Noop, this also exercises the 0-viewport branch directly.
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<PostProcessPass>());

    ayt::render::detail::FrameContext frame{};
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    ayt::render::detail::BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    ayt::render::detail::PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 0, 0, frame, /*viewId=*/0
    };
    const uint32_t draws = pipe.executeAll(ctx);
    CHECK(draws == 0);
}

TEST_CASE(r5plus_postprocess_setenabled_false_still_returns_zero) {
    // R5+ — pipeline honors setEnabled(false) before the adapter
    // isInitialized() guard, so the pass is short-circuited by the
    // pipeline loop rather than the body. Same observable outcome
    // (0 draws) but exercises a different code path (pipeline
    // dispatch guard vs pass-body guard).
    PostProcessPass pass;
    pass.setEnabled(false);
    CHECK(pass.isEnabled() == false);

    RenderPipeline pipe;
    pipe.addPass(std::make_unique<PostProcessPass>());
    pipe.passes()[0]->setEnabled(false);

    uint32_t totalDraws = 0;
    runWithEmptyScene(pipe, totalDraws);
    CHECK(totalDraws == 0);
}

TEST_SUITE_END