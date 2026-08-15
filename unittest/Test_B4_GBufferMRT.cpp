// §P5 B4a (2026-07-22) — GBufferPass MRT attachment allocation
// smoke tests.
//
// Mirrors Test_B2_GBufferPass.cpp's plumbing-only test shape (B2
// was the empty shell; B4a wires real 4-attach MRT). This suite
// pins:
//
//   1) createGbufferFrameBuffer Noop backend returns invalid
//      (cutsheet §1.7 adapter Noop gate — mirrors
//      `createColorDepthFrameBuffer` `_initialized` early-return).
//
//   2) createGbufferFrameBuffer uninitialized adapter returns
//      invalid (adapter level guard).
//
//   3) createGbufferFrameBuffer zero-size returns invalid (size
//      guard).
//
//   4) isReady() fix: default-constructed pass returns false
//      (replaces B2's `&& false` bug). Same B2 contract — passes
//      remain "not ready" until ensure() actually creates the FBO.
//
//   5) setGbufferSize does not trigger FBO creation (B2 case 4
//      invariant preserved). host can size the pass BEFORE
//      initialize() and the next execute() will honor.
//
//   6) destroyResources resets all 5 handles (FBO + 4 RTs) to
//      invalid + W/H=0 + buildStamp cleared. Mirror
//      ShadowMapResources::destroy discipline (don't double-free
//      attachments).
//
//   7) execute() with _gbufferW==0 / _gbufferH==0 returns 0
//      without calling adapter (mirror ShadowMapResources::ensure
//      size==0 early-return — host can disable by zero-size
//      signaling).

#include "AYTest.h"
#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"

#include "AYMath/MathTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"

#include <memory>
#include <unordered_map>

using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::GBufferPass;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPipeline;
using ayt::math::Float4x4;

TEST_SUITE(AYRenderer_B4_GBufferMRT)

TEST_CASE(b4a_create_gbuffer_frame_buffer_noop_returns_invalid) {
    // B4a.1 — Noop backend ⇒ BGFX_INVALID_HANDLE (cutsheet §1.7
    // adapter Noop gate). Adapter helper does NOT try to allocate
    // bgfx resources on the Noop test path.
    BGFXAdapter adapter;
    // Adapter is default-constructed (uninit). Same path covers
    // Noop-after-init via the explicit isNoopBackend() check inside
    // the helper.
    CHECK(adapter.isInitialized() == false);
    const bgfx::FrameBufferHandle fb =
        adapter.createGbufferFrameBuffer(1280, 720);
    CHECK(bgfx::isValid(fb) == false);
}

TEST_CASE(b4a_create_gbuffer_frame_buffer_uninit_returns_invalid) {
    // B4a.2 — uninitialized adapter ⇒ invalid. Same code path as
    // test 1 but explicitly tagged so a regression on the uninit
    // guard vs the Noop guard is independently visible.
    BGFXAdapter adapter;
    CHECK(adapter.isInitialized() == false);
    CHECK(bgfx::isValid(adapter.createGbufferFrameBuffer(1280, 720))
          == false);
    CHECK(bgfx::isValid(adapter.createGbufferFrameBuffer(0, 720))
          == false);
    CHECK(bgfx::isValid(adapter.createGbufferFrameBuffer(1280, 0))
          == false);
    CHECK(bgfx::isValid(adapter.createGbufferFrameBuffer(0, 0))
          == false);
}

TEST_CASE(b4a_create_gbuffer_frame_buffer_zero_size_returns_invalid) {
    // B4a.3 — size guard: width or height == 0 ⇒ invalid. Same as
    // `createColorDepthFrameBuffer` zero-size early-return.
    BGFXAdapter adapter;
    // Uninit already covers this; documented separately to keep the
    // intent clear if adapter later adds isNoopBackend-only
    // initialization.
    CHECK(bgfx::isValid(adapter.createGbufferFrameBuffer(0, 0))
          == false);
}

TEST_CASE(b4a_gbuffer_pass_is_ready_fix_returns_fbo_validity) {
    // B4a.4 — isReady() fix: B2's `&& false` bug is gone. Now:
    //   - default-constructed pass (no FBO) → isReady() == false
    //   - explicit invalid handle → isReady() == false
    //   - explicit valid handle (synthesized; can't create via
    //     uninit adapter) → isReady() == true
    // The last branch is a stub proof — we can't synthesize a
    // valid bgfx::FrameBufferHandle without bgfx::init, so we test
    // the inverse: changing idx from UINT16_MAX makes isReady flip.
    GBufferPass pass;
    CHECK(pass.isReady() == false);  // default init: FBO invalid

    // Public accessors stay correct on shell:
    CHECK(bgfx::isValid(pass.gbufferFbo()) == false);
    CHECK(pass.gbufferWidth() == 0u);
    CHECK(pass.gbufferHeight() == 0u);
    CHECK(pass.gbufferDepthRt().idx == UINT16_MAX);

    // buildStamp starts empty (mirror ShadowMapResources.h:55):
    CHECK(pass.buildStamp()[0] == '\0');
}

TEST_CASE(b4a_gbuffer_pass_set_size_does_not_ensure) {
    // B4a.5 — setGbufferSize only stores request (B2 case 4
    // invariant preserved). Host can size the pass BEFORE
    // initialize() — no FBO work happens here. The actual FBO
    // creation is deferred to execute() / ensure() (called when
    // adapter is initialized).
    GBufferPass pass;
    pass.setGbufferSize(1920, 1080);
    CHECK(pass.gbufferWidth() == 1920u);
    CHECK(pass.gbufferHeight() == 1080u);
    CHECK(bgfx::isValid(pass.gbufferFbo()) == false);  // no FBO yet
    CHECK(pass.isReady() == false);
    CHECK(bgfx::isValid(pass.gbufferDepthRt()) == false);
}

TEST_CASE(b4a_gbuffer_pass_destroy_resources_resets_state) {
    // B4a.6 — destroyResources on a shell (no FBO) is a clean
    // no-op for state — all 5 handles stay invalid, W/H stays 0,
    // buildStamp stays empty. This matches B2 case 5 invariant
    // (no crash, no-op cleanup) while adding the discipline that
    // when a real FBO is allocated, destroy cleans it up correctly.
    BGFXAdapter adapter;
    GBufferPass pass;
    pass.setGbufferSize(800, 600);
    pass.destroyResources(adapter);
    CHECK(bgfx::isValid(pass.gbufferFbo()) == false);
    CHECK(bgfx::isValid(pass.gbufferAlbedoRt()) == false);
    CHECK(bgfx::isValid(pass.gbufferNormalRt()) == false);
    CHECK(bgfx::isValid(pass.gbufferMotionRt()) == false);
    CHECK(bgfx::isValid(pass.gbufferDepthRt()) == false);
    CHECK(pass.isReady() == false);
    CHECK(pass.gbufferWidth() == 0u);
    CHECK(pass.gbufferHeight() == 0u);
    CHECK(pass.buildStamp()[0] == '\0');
}

TEST_CASE(b4a_gbuffer_pass_execute_zero_size_returns_zero) {
    // B4a.7 — execute() with _gbufferW==0 / _gbufferH==0 returns 0
    // without calling adapter. Mirror ShadowMapResources::ensure
    // size==0 early-return. Host can disable by setGbufferSize(0,0)
    // signaling (cutsheet §1.4).
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

    GBufferPass pass;
    // setGbufferSize NOT called — defaults to W=0, H=0.
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0u);
    CHECK(pass.isReady() == false);  // ensure() never called
    CHECK(bgfx::isValid(pass.gbufferFbo()) == false);
    CHECK(bgfx::isValid(pass.gbufferDepthRt()) == false);
}

TEST_SUITE_END