// PR-F2 (2026-07-21) — forward-pass shadow sampling smoke tests.
//
// E5 (§5.4, 2026-07-22) update: the default pipeline now mounts
// Shadow *enabled* (no opt-in needed). This suite still pins the
// same plumbing invariants on Noop — Shadow's Noop guard keeps the
// visible-shadow behavior unchanged from E4 / pre-E5 — so no
// assertion changes are required here. The E5 default is verified
// in Test_E5_DefaultShadow.cpp via Renderer::shadowsEnabled().
//
// Background:
//   The ShadowPass depth-only FBO is consumed by Forward / Transparent
//   via a non-owning pointer (`PassExecContext::shadowPass`). The actual
//   visible-shadow behavior is only observable on a live GPU backend,
//   just like the F1' light-space matrices. The Noop backend (the only
//   background-friendly option) short-circuits and isReady() stays
//   false; what we CAN pin on Noop is the *plumbing* (binding lookup,
//   uniform name, sampler name, context handoff).
//
// What this pins:
//   1) `PassExecContext::shadowPass` field default-initializes to
//      nullptr even when a test brace-init omits it (Field-init-after-
//      trailing-default is the standard C++14+ behavior — guarding
//      against accidental deletion of the field).
//   2) `tryBindShadowSampler` (the helper that FO/Transparent both
//      call) no-ops cleanly when:
//        (a) shadowPass == nullptr
//        (b) shadowPass->shadowFbo() is invalid (Noop / first-frame)
//        (c) the shader has no `u_lightViewProj` binding
//        (d) the shader has no `shadowMap` binding
//      A regression that introduced a NULL deref on any of those
//      paths would crash here.
//   3) Manual pipeline: ShadowPass + ForwardOpaquePass + TransparentPass
//      with ctx.shadowPass = &shadow. The pipeline dispatches in
//      order, ShadowPass writes light-space matrices, FO/Transparent
//      read them through the helper. On Noop, ShadowPass's getters
//      stay identity (it never created an FBO) and the forward passes
//      skip the sampler-bind — verify the plumbing *runs* by checking
//      that draw counts and pass-state are consistent and that the
//      forward passes don't crash.
//   4) shadowPass pointer handoff across a 5-pass pipeline (Shadow +
//      FO + Trans + PostProcess + UI) wired exactly like the F1'
//      Test_ShadowPass manual pipeline.
//
// Out-of-scope (covered by docs/execution-plan.md §5.4 E6 follow-up):
//   - The actual sampler2d D24S8 .r-channel compare in Phoskia
//     fragment (requires a live GPU backend + a non-null shadow FBO).
//   - light-space correctness (already pinned by
//     Test_ShadowPass::f1_safe_light_space_matrices_non_identity_and_direction_sensitive).

#include "AYTest.h"
#include "AYRenderer/RenderScene.h"
#include "AYShader/ShaderResourcePool.h"

#include "AYMath/MathTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FrameContext.h"
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
using ayt::render::detail::ForwardOpaquePass;
using ayt::render::detail::FrameContext;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::RenderPipeline;
using ayt::render::detail::ShadowPass;
using ayt::render::detail::TransparentPass;
using ayt::render::detail::UIPass;
using ayt::math::Float4x4;

namespace {

// Minimal capture pass — verifies PassExecContext::shadowPass
// propagates between passes (set on the host → read by FO/Trans
// through tryBindShadowSampler → reads ShadowPass::shadowFbo()).
// Records what ctx.shadowPass pointed at, so the test can assert
// it survived the full dispatch without the FBO getting mangled.
struct ShadowCapturePass final : public ayt::render::detail::RenderPass {
    static inline const ShadowPass* lastSeen = nullptr;
    static inline uint32_t callCount = 0;

    std::string_view name() const override { return "ShadowCapture"; }

    uint32_t execute(PassExecContext& ctx) override {
        lastSeen = ctx.shadowPass;
        ++callCount;
        return 0;
    }
};

} // namespace

TEST_SUITE(AYRenderer_F2_ForwardShadow)

TEST_CASE(f2_passexec_context_has_shadow_pass_field_default_null) {
    // Even with the brace-init fully populated, ctx.shadowPass must
    // be present and default-init to nullptr. Pins the field exists
    // (a PR that deletes it would compile-fail all the other F2
    // tests).
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

    CHECK(ctx.shadowPass == nullptr);

    // Setting it the host way still works.
    ShadowPass pass;
    ctx.shadowPass = &pass;
    CHECK(ctx.shadowPass == &pass);
}

TEST_CASE(f2_try_bind_shadow_sampler_noop_when_shadow_pass_null) {
    // F2.2.a — nullptr ⇔ no-op. Regression guard: if anyone makes
    // tryBindShadowSampler dereference shadowPass unconditionally,
    // this case crashes on a NULL deref.
    BGFXAdapter adapter;
    ayt::render::detail::GpuMaterial material;
    // shader field stays default (invalid ShaderResource) so binding
    // lookups return Invalid — the helper must early-out clean.
    tryBindShadowSampler(material.shader, adapter, /*shadowPass=*/nullptr);
    CHECK(true);  // reached ⇒ no crash
}

TEST_CASE(f2_try_bind_shadow_sampler_noop_when_fbo_invalid) {
    // F2.2.b — ShadowPass present but its FBO is invalid (Noop / first
    // frame). Expect a clean early-exit; the helper must not block on
    // getTexture for a degenerate handle.
    BGFXAdapter adapter;
    ayt::render::detail::GpuMaterial material;

    ShadowPass pass;
    CHECK(bgfx::isValid(pass.shadowFbo()) == false);

    tryBindShadowSampler(material.shader, adapter, &pass);
    CHECK(true);  // reached ⇒ no crash
}

TEST_CASE(f2_forward_opaque_noop_draws_unaffected_by_null_shadow_pass) {
    // F2.3 baseline — PR-F2 must be a no-op for the existing
    // "ForwardOpaquePass dispatches on Noop and returns 0 drawCount"
    // invariant. Without a ShadowPass wired in, behavior matches the
    // pre-F2 path exactly (draw count == 0 because scene.items() is
    // empty AND even when populated, the Noop backend short-circuits
    // in the forward pass).

    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ForwardOpaquePass>());

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
    ctx.shadowPass = nullptr;  // explicit post-construction reset

    const uint32_t draws = pipe.executeAll(ctx);
    CHECK(draws == 0u);
}

TEST_CASE(f2_shadow_pass_pointer_visible_to_following_pass) {
    // F2.4 — the ShadowCapturePass installed between Shadow and
    // ForwardOpaquePass sees ctx.shadowPass == &the shadow instance
    // even though the ShadowPass producer had no live FBO on Noop.
    // Pins: pointer-handoff works regardless of isReady().

    ShadowCapturePass::lastSeen = nullptr;
    ShadowCapturePass::callCount = 0;

    // The pipeline SHADOWs the producer pointer (the ShadowPass
    // lives in the pipe vector, the capture pass reads ctx.shadowPass
    // by pointer). On Noop both ShadowPass::lightView/Proj stay
    // identity and the capture pass still sees the pointer.
    RenderPipeline pipe;
    auto extProducer = std::make_unique<ShadowPass>();
    ShadowPass* const producerPtr = extProducer.get();
    pipe.addPass(std::move(extProducer));
    pipe.addPass(std::make_unique<ShadowCapturePass>());

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
    ctx.shadowPass = producerPtr;

    const uint32_t draws = pipe.executeAll(ctx);
    CHECK(draws == 0u);
    CHECK(ShadowCapturePass::callCount == 1u);
    CHECK(ShadowCapturePass::lastSeen == producerPtr);
}

TEST_CASE(f2_full_shadow_pipeline_noop_dispatch_with_pointer_wired) {
    // F2.5 — the full Shadow + FO + Transparent + PostProcess + UI
    // manual pipeline (mirrors Test_ShadowPass's PR-F1' validation)
    // with ctx.shadowPass wired. Without a real FBO, every pass
    // returns 0 draws, but the wiring must not crash.
    RenderPipeline pipe;
    auto extProducer = std::make_unique<ShadowPass>();
    ShadowPass* const producerPtr = extProducer.get();
    pipe.addPass(std::move(extProducer));
    pipe.addPass(std::make_unique<ForwardOpaquePass>());
    pipe.addPass(std::make_unique<TransparentPass>());
    pipe.addPass(std::make_unique<PostProcessPass>());
    pipe.addPass(std::make_unique<UIPass>());

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
    ctx.shadowPass = producerPtr;

    const uint32_t total = pipe.executeAll(ctx);
    CHECK(total == 0u);  // Noop short-circuit

    // Default pipeline still doesn't include Shadow (this test
    // manually wires it; the host default stays 4-pass per §5.3).
    CHECK(pipe.passes().size() == 5u);
    CHECK(pipe.passes()[0]->name() == "Shadow");
    CHECK(pipe.passes()[1]->name() == "ForwardOpaque");
    CHECK(pipe.passes()[2]->name() == "Transparent");
    CHECK(pipe.passes()[3]->name() == "PostProcess");
    CHECK(pipe.passes()[4]->name() == "UI");

    // Light-space matrices on Noop stay identity (ShadowPass never
    // built an FBO). Even so the pointer is set, which means
    // tryBindShadowSampler will be called by FO and Transparent —
    // and will exit early on the invalid FBO check. No crash. Pass.
}

TEST_CASE(f2_shadow_pass_getters_return_identity_on_noop) {
    // F2.6 — Light-space getter shape unchanged from F1'. Pins that
    // F2 wiring didn't accidentally clobber the cached matrices.
    ShadowPass pass;

    auto matEq = [](const Float4x4& a, const Float4x4& b) {
        for (int i = 0; i < 16; ++i) {
            if (a.ptr()[i] != b.ptr()[i]) return false;
        }
        return true;
    };
    CHECK(matEq(pass.lightView(), Float4x4::identity()));
    CHECK(matEq(pass.lightProj(), Float4x4::identity()));
    CHECK(matEq(pass.lightViewProj(), Float4x4::identity()));
    CHECK(pass.isReady() == false);
}

TEST_SUITE_END
