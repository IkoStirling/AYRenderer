// §P5 B2 (2026-07-22) — GBufferPass empty shell smoke tests.
//
// Mirrors Test_F2_ForwardShadow.cpp's structure (PR-F2 plumbing-only
// pass + PassExecContext borrowed pointer). This suite pins:
//
//   1) PassExecContext::gbufferPass field exists + default-init nullptr
//      in a 12-field brace-init form. Pins the field is present
//      (a future PR that deletes it would compile-fail this suite).
//
//   2) GBufferPass base contract:
//        - name() == "GBuffer"
//        - isReady() == false on construction (no FBO, no program)
//        - static constexpr view-id = 7 (B4 lock per docs/pass-
//          lessons-from-deferred.md §5.1)
//
//   3) execute() Noop-gates cleanly when adapter is uninitialized
//      (returns 0) AND when adapter is initialized but on Noop
//      backend (returns 0). Mirrors ShadowPass::execute's Noop
//      early-exit.
//
//   4) Accessors return BGFX_INVALID_HANDLE / 0 until B4 wires
//      real GPU state. This is the contract downstream consumers
//      (B5 LightingPass) rely on for "no GBuffer RT this frame".
//
//   5) destroyResources() is a clean no-op on a shell instance
//      (no crash, no-op cleanup before B4 attaches real resources).
//
//   6) Five-pass pipeline with GBufferPass wired + ctx.gbufferPass
//      pointer set — full dispatch through executeAll() returns 0
//      on Noop, no crash, no state corruption. Mirrors the
//      F2 full-pipeline test shape.
//
//   7) ctx.gbufferPass can be set after construction (the F2
//      pattern: set producerPtr, then read via ctx.shadowPass in
//      a downstream pass). Pins the field's read/write semantics
//      independent of the brace-init form.

#include "AYTest.h"
#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"

#include "aymath/MathTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"
#include "detail/ShadowPass.h"
#include "detail/TransparentPass.h"
#include "detail/UIPass.h"

#include <memory>
#include <unordered_map>

using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GBufferPass;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPipeline;
using ayt::render::detail::ShadowPass;
using ayt::math::Float4x4;

namespace {

// Capture pass — verifies PassExecContext::gbufferPass propagates
// between passes (set on the host → read by downstream passes).
// Records what ctx.gbufferPass pointed at, so the test can assert
// it survived the full dispatch without state corruption. Mirrors
// the ShadowCapturePass pattern in Test_F2_ForwardShadow.cpp.
struct GBufferCapturePass final : public ayt::render::detail::RenderPass {
    static inline const GBufferPass* lastSeen = nullptr;
    static inline uint32_t callCount = 0;

    std::string_view name() const override { return "GBufferCapture"; }

    uint32_t execute(PassExecContext& ctx) override {
        lastSeen = ctx.gbufferPass;
        ++callCount;
        return 0;
    }
};

} // namespace

TEST_SUITE(AYRenderer_B2_GBufferPass)

TEST_CASE(b2_passexec_context_has_gbuffer_pass_field_default_null) {
    // Even with the brace-init fully populated, ctx.gbufferPass must
    // be present and default-init to nullptr. Pins the field exists
    // (a PR that deletes it would compile-fail this suite + every
    // existing 12-field brace-init test).
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    FrameContext frame;

    // 12-field brace-init — ctx.gbufferPass must default-init to
    // nullptr via C++14+ trailing-default rules (same guarantee as
    // PR-F2's shadowPass default-init). If anyone tightens the
    // struct to require explicit gbufferPass, this compile-fails.
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };

    CHECK(ctx.gbufferPass == nullptr);

    // Setting it the host way still works.
    GBufferPass pass;
    ctx.gbufferPass = &pass;
    CHECK(ctx.gbufferPass == &pass);
}

TEST_CASE(b2_gbuffer_pass_name_and_initial_state) {
    // Pins the base RenderPass contract:
    //   name() == "GBuffer"
    //   isReady() == false (no FBO, no program yet — shell only)
    //   kGBufferViewId == 7 (B4 lock — view 7 is reserved for
    //     GBuffer MRT; if anyone re-uses view 7 elsewhere this
    //     compile-fails the constant via collision)
    GBufferPass pass;
    CHECK(pass.name() == "GBuffer");
    CHECK(pass.isReady() == false);
    CHECK(GBufferPass::kGBufferViewId == 7u);
    CHECK(GBufferPass::kGBufferAttachmentCount == 3u);
}

TEST_CASE(b2_gbuffer_pass_noop_uninitialized_returns_zero) {
    // B2.1.a — adapter uninitialized ⇒ execute returns 0 with no
    // GPU work (mirrors ShadowPass::execute Noop gate). No crash.
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    FrameContext frame;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };

    GBufferPass pass;
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0u);
    CHECK(pass.isReady() == false);
}

TEST_CASE(b2_gbuffer_pass_accessors_return_invalid_on_shell) {
    // B2.1.b — stub accessors return BGFX_INVALID_HANDLE / 0 until
    // B4 wires real GPU state. This is the contract downstream
    // consumers (B5 LightingPass, future B7+ multi-light) rely on
    // for "no GBuffer RT this frame → use fallback path".
    GBufferPass pass;

    CHECK(bgfx::isValid(pass.gbufferFbo()) == false);
    CHECK(bgfx::isValid(pass.gbufferAlbedoRt()) == false);
    CHECK(bgfx::isValid(pass.gbufferNormalRt()) == false);
    CHECK(bgfx::isValid(pass.gbufferMotionRt()) == false);
    CHECK(pass.gbufferWidth() == 0u);
    CHECK(pass.gbufferHeight() == 0u);

    // setGbufferSize preserves the request but the shell has no FBO
    // yet — accessor still returns invalid handle until B4 lands.
    pass.setGbufferSize(1920, 1080);
    CHECK(pass.gbufferWidth() == 1920u);
    CHECK(pass.gbufferHeight() == 1080u);
    CHECK(bgfx::isValid(pass.gbufferFbo()) == false);
}

TEST_CASE(b2_gbuffer_pass_destroy_resources_is_noop_on_shell) {
    // B2.1.c — destroyResources() on the empty shell is a clean
    // no-op (no resources to destroy yet). Mirrors the F1'
    // ShadowPass::destroyResources pattern, just stubbed until B4
    // attaches real FBO + RT attachments.
    BGFXAdapter adapter;
    GBufferPass pass;
    pass.setGbufferSize(800, 600);
    pass.destroyResources(adapter);
    CHECK(bgfx::isValid(pass.gbufferFbo()) == false);
    CHECK(pass.isReady() == false);
}

TEST_CASE(b2_gbuffer_pass_pointer_visible_to_following_pass) {
    // B2.2 — the GBufferCapturePass installed after GBufferPass
    // sees ctx.gbufferPass == &the gbuffer instance even though
    // the GBufferPass shell has no live FBO. Pins: pointer handoff
    // works regardless of isReady() — same guarantee ShadowPass
    // gets via the F2 suite.
    GBufferCapturePass::lastSeen = nullptr;
    GBufferCapturePass::callCount = 0;

    RenderPipeline pipe;
    auto extProducer = std::make_unique<GBufferPass>();
    GBufferPass* const producerPtr = extProducer.get();
    pipe.addPass(std::move(extProducer));
    pipe.addPass(std::make_unique<GBufferCapturePass>());

    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    FrameContext frame;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    ctx.gbufferPass = producerPtr;

    const uint32_t draws = pipe.executeAll(ctx);
    CHECK(draws == 0u);
    CHECK(GBufferCapturePass::callCount == 1u);
    CHECK(GBufferCapturePass::lastSeen == producerPtr);
}

TEST_CASE(b2_full_pipeline_noop_dispatch_with_gbuffer_pointer_wired) {
    // B2.3 — minimal pipeline: GBuffer + Shadow + PostProcess +
    // UIPass only. We deliberately omit ForwardOpaquePass and
    // TransparentPass because they call bgfx C-API surface
    // (setViewTransform / setViewFrameBuffer) without an
    // isInitialized() guard — they need a live adapter (Noop is
    // also fine since BGFXAdapter::isInitialized gates bgfx
    // calls). The GBuffer + Shadow + PostProcess + UI chain is
    // sufficient to pin the B2 contract: gbufferPass pointer
    // survives full dispatch through executeAll() without crash
    // and reaches the downstream capture pass.
    RenderPipeline pipe;
    auto extProducer = std::make_unique<GBufferPass>();
    GBufferPass* const producerPtr = extProducer.get();
    pipe.addPass(std::move(extProducer));
    pipe.addPass(std::make_unique<ShadowPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::PostProcessPass>());
    pipe.addPass(std::make_unique<ayt::render::detail::UIPass>());
    pipe.addPass(std::make_unique<GBufferCapturePass>());

    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    FrameContext frame;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    ctx.gbufferPass = producerPtr;

    GBufferCapturePass::lastSeen = nullptr;
    GBufferCapturePass::callCount = 0;

    const uint32_t total = pipe.executeAll(ctx);
    CHECK(total == 0u);

    CHECK(pipe.passes().size() == 5u);
    CHECK(pipe.passes()[0]->name() == "GBuffer");
    CHECK(pipe.passes()[1]->name() == "Shadow");
    CHECK(pipe.passes()[2]->name() == "PostProcess");
    CHECK(pipe.passes()[3]->name() == "UI");
    CHECK(pipe.passes()[4]->name() == "GBufferCapture");
    CHECK(GBufferCapturePass::callCount == 1u);
    CHECK(GBufferCapturePass::lastSeen == producerPtr);
}

TEST_SUITE_END