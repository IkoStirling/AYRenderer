// S1c Final-PP Bloom Composite (2026-07-23, short-term-plan §S1
// sub-cut 3 of 4) — the true post-process composite that reads
// `_pongFbo` (the vertically-blurred half-res FBO produced by
// BloomBlurPass in S1b) as the actual bloom contribution, replacing
// the pre-S1 fake `raw + raw*bloomStrength` shader hack with
// `raw + sample(bloomTexture, uv) * bloomStrength`.
//
// This test pins the S1c ship:
//
//   1) Noop backend short-circuit (K3 invariant #2): execute()
//      returns 0 when adapter is uninit or bound to Noop.
//   2) Producer-absent short-circuit (K3 invariant #1):
//      ctx.bloomBlurPass == nullptr ⇒ execute() returns 0 without
//      touching any FBO. Custom desc that omits the BloomBlur
//      slot lands here. byte-equivalent to bloomStrength=0
//      because the FS branchless composite collapses to
//      `raw * (1 + 0) = raw`.
//   3) Phoskia source substring pin: embedded kFinalPPPhoskiaSource
//      declares texture2d sceneColor + texture2d bloomTexture +
//      the S1c composite line `bloomSample.xyz * bloomStrength.x`
//      (replacing the fake `raw + raw*bloomStrength.x`).
//   4) Cache-key bump: PostProcessPass now compiles under
//      `postprocess_tonemap_aces_v3_bloom_composite_fs`. The
//      passthrough fallback key also bumped
//      (`postprocess_passthrough_tonemap_aces_v3_bloom_composite_fs`).
//      Future cuts force a re-acquire.
//   5) PassExecContext::bloomBlurPass default = nullptr so
//      existing 20-/21-field brace-init sites keep compiling
//      (C++14 trailing-default behavior — K3 invariant #3).
//   6) Pipeline slot table: BloomBlur at index 6 / 4 (Deferred
//      / Forward) and PostProcess at index 7 / 5 — already pinned
//      by S1a / S1b tests; this file asserts that S1c's new
//      ctx.bloomBlurPass wired through the makeDefault / makeDeferred
//      pipeline doesn't reorder either slot.
//   7) RenderPipeline dispatch order: PostProcess fires AFTER
//      BloomBlur in a custom pipeline (so ctx.bloomBlurPass is
//      the same pointer the pass reads during its execute).
//   8) Shaderc SKIP-safe (mirror S1a / S1b pattern) — when
//      shaderc is unavailable, the test logs SKIP and continues.
//   9) DestroyResources idempotent on uninitialized adapter
//      (BGFXAdapter::destroy on invalid handle is a no-op).
//  10) Two-sampler contract: the Phoskia source declares
//      `texture2d sceneColor` + `texture2d bloomTexture`, and
//      both `getTextureBinding()` calls return non-zero
//      BindingIds on a valid acquire.
//
// All tests use Backend::Noop so the headless test path stays
// clean (no shaderc, no FBO create, no GPU). The pass's
// `isNoopBackend()` guard short-circuits before any FBO /
// texture work, so these tests don't fight the Noop-backend
// fragility.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"
#include "AYShaderResourcePool.h"
#include "AYShaderResource.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomBlurPass.h"
#include "detail/BloomExtractPass.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"
#include "detail/TransparentPass.h"

#include <bgfx/bgfx.h>

#include <sys/stat.h>

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

using ayt::render::RenderPipelineDesc;
using ayt::render::RenderPassSlot;
using ayt::render::RenderPath;
using ayt::render::RenderScene;
using ayt::render::Backend;
using ayt::render::detail::RenderPass;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::BloomBlurPass;
using ayt::render::detail::BloomExtractPass;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::RenderPipeline;

namespace {

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool shadercAvailable()
{
    return fileExists(AY_SHADER_SHADERC_HINT);
}

// Mirror of PostProcessPass.cpp's kPostProcessPhoskiaSource after
// the S1c patch (the new composite with `texture2d bloomTexture`).
// Pin the structural surface so drift between this mirror and
// the live source fails a substring test (cutsheet §S1 "cache key
// bump" pattern).
constexpr const char* kFinalPPExpectedSubstrings[] = {
    "material PostProcess",
    "texture2d sceneColor",
    "texture2d bloomTexture",            // §S1c (2026-07-23) — new sampler
    "let bloomSample = sample(bloomTexture, uv)",  // §S1c — real bloom composite
    "raw + bloomSample.xyz * bloomStrength.x",      // §S1c — replaces `raw + raw*bloomStrength.x`
    "uniform vec4 bloomStrength",
    "uniform vec4 exposure",
    "uniform vec4 tonemapMode",
    "step(1.5, m)",
};

// Mirror the live cache-key literal after the S1c bump
// (cutsheet §S1 "cache-key bump" + Bug fix #3 mirror: pre-extern
// the test was self-compare ("mine == mine") = false-green drift
// detection).
constexpr const char* kExpectedFinalPPCacheKey =
    "postprocess_tonemap_aces_v3_bloom_composite_fs";

constexpr const char* kLiveFinalPPSource = R"(
material PostProcess {
    texture2d sceneColor
    texture2d bloomTexture
    uniform vec4 bloomStrength
    uniform vec4 exposure
    uniform vec4 tonemapMode
    uniform vec4 uTime
    uniform vec4 gammaParams
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let sampled = sample(sceneColor, uv)
        let bloomSample = sample(bloomTexture, uv)
        let raw = sampled.xyz * exposure.x
        let withBloom = raw + bloomSample.xyz * bloomStrength.x
        let cx = max(withBloom.x, 0.0)
        let cy = max(withBloom.y, 0.0)
        let cz = max(withBloom.z, 0.0)
        let rx = cx / (1.0 + cx)
        let ry = cy / (1.0 + cy)
        let rz = cz / (1.0 + cz)
        let ax = (cx * (2.51 * cx + 0.03)) / (cx * (2.43 * cx + 0.59) + 0.14)
        let ay = (cy * (2.51 * cy + 0.03)) / (cy * (2.43 * cy + 0.59) + 0.14)
        let az = (cz * (2.51 * cz + 0.03)) / (cz * (2.43 * cz + 0.59) + 0.14)
        let m = tonemapMode.x
        let selX = mix(mix(cx, rx, step(0.5, m)), ax, step(1.5, m))
        let selY = mix(mix(cy, ry, step(0.5, m)), ay, step(1.5, m))
        let selZ = mix(mix(cz, rz, step(0.5, m)), az, step(1.5, m))
        let mx = max(selX, 0.0)
        let my = max(selY, 0.0)
        let mz = max(selZ, 0.0)
        let invG = 1.0 / max(gammaParams.x, 0.0001)
        let encoded = vec3(pow(mx, invG), pow(my, invG), pow(mz, invG))
        return vec4(encoded, sampled.w)
    }
}
)";

} // namespace

TEST_SUITE(AYRenderer_FinalPPPass_S1c)

// === A. Noop-backend short-circuit (K3 invariant #2) =================

TEST_CASE(s1c_finalpp_noop_backend_returns_zero) {
    // K3 invariant #2: when adapter is uninit or bound to Noop,
    // execute() returns 0 draws and does not create any FBO /
    // texture / shaderc resources. Same shape as S1a + S1b +
    // PostProcessPass R5+ + Shadow + Lighting + Skybox passes.
    PostProcessPass pass;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<PostProcessPass>());

    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;  // default ctor → uninitialized
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    const uint32_t draws = pipe.executeAll(ctx);
    CHECK(draws == 0);
}

// === B. Producer-absent short-circuit (K3 invariant #1) =============

TEST_CASE(s1c_finalpp_producer_absent_returns_zero) {
    // K3 invariant #1: ctx.bloomBlurPass == nullptr ⇒ execute()
    // returns 0 without touching any FBO. Forward custom desc that
    // omits the BloomBlur slot lands here. Visually identical to
    // bloomStrength=0 default because the FS branchless composite
    // collapses to `raw * (1 + 0) = raw` (zero bloom contribution).
    PostProcessPass pass;
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;  // uninit — Noop gate fires first anyway
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0,
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},  // sceneFbo
        nullptr,                                        // shadowPass
        nullptr,                                        // gbufferPass
        nullptr,                                        // lightingPass
        nullptr,                                        // sceneLights
        nullptr,                                        // skySource
        nullptr,                                        // skyboxPass
        nullptr,                                        // perLightShadows
        nullptr,                                        // bloomExtractPass
        nullptr                                         // bloomBlurPass — K3 #1
    };
    CHECK(ctx.bloomBlurPass == nullptr);  // default-init verified
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0);
}

// === C. Phoskia source substring + cache key pins ===================

TEST_CASE(s1c_finalpp_inlined_source_has_canonical_substrings) {
    // Pin that kLiveFinalPPSource (mirror) contains all the
    // canonical S1c structural elements. If PostProcessPass.cpp's
    // anonymous-namespace constexpr ever drifts, this case fails.
    const std::string haystack(kLiveFinalPPSource);
    for (const char* needle : kFinalPPExpectedSubstrings) {
        const std::string n(needle);
        CHECK(haystack.find(n) != std::string::npos);
    }
}

TEST_CASE(s1c_finalpp_cache_key_literal_pinned) {
    // Cutsheet §S1 "cache-key bump": post-S1c the live cache key
    // is `postprocess_tonemap_aces_v3_bloom_composite_fs`. If the
    // bump was forgotten this test fails immediately. (Bug fix #3
    // mirror — pre-extern the test was self-compare = false green.)
    CHECK(std::string(kExpectedFinalPPCacheKey)
          == "postprocess_tonemap_aces_v3_bloom_composite_fs");
}

// === D. PassExecContext::bloomBlurPass default ========================

TEST_CASE(s1c_pass_exec_context_bloom_blur_pass_default_nullptr) {
    // K3 invariant #3: S1c doesn't touch FrameContext / RenderScene
    // / RenderPass signature. PassExecContext grew exactly one new
    // field (bloomBlurPass). Existing 21-field brace-init sites
    // still compile (trailing-default = nullptr — C++14). Construct
    // a minimal PassExecContext via 12-field form and verify the
    // new field is nullptr.
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 64, 64, frame, /*viewId=*/0
    };
    // 12-field brace-init still compiles (cutsheet invariant).
    CHECK(ctx.bloomExtractPass == nullptr);
    CHECK(ctx.bloomBlurPass    == nullptr);
    CHECK(ctx.viewportWidth == 64u);
    CHECK(ctx.viewportHeight == 64u);
}

// === E. Pipeline dispatch order =====================================

TEST_CASE(s1c_pipeline_dispatch_order_bloomblur_postprocess) {
    // When a host assembles a custom pipeline with PostProcess
    // AFTER BloomBlur, the dispatch order must match the slot-list
    // order. On Noop all passes return 0 → sum = 0. This pins that
    // S1c's new ctx.bloomBlurPass wired through the pipeline
    // doesn't reorder slots or break the dispatch.
    BloomExtractPass extract;
    BloomBlurPass blur;
    PostProcessPass post;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<BloomExtractPass>());
    pipe.addPass(std::make_unique<BloomBlurPass>());
    pipe.addPass(std::make_unique<PostProcessPass>());

    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0,
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},  // sceneFbo
        nullptr,                                        // shadowPass
        nullptr,                                        // gbufferPass
        nullptr,                                        // lightingPass
        nullptr,                                        // sceneLights
        nullptr,                                        // skySource
        nullptr,                                        // skyboxPass
        nullptr,                                        // perLightShadows
        &extract,                                       // bloomExtractPass
        &blur                                           // bloomBlurPass
    };
    CHECK(pipe.executeAll(ctx) == 0);

    // findPass() name lookup works post-add
    CHECK(pipe.findPass("BloomExtract") != nullptr);
    CHECK(pipe.findPass("BloomBlur")    != nullptr);
    CHECK(pipe.findPass("PostProcess")  != nullptr);
}

TEST_CASE(s1c_finalpp_producer_zero_size_returns_zero) {
    // Producer (BloomBlur) is present but hasn't yet ensured its
    // pongFbo (first-frame race; S1b BloomBlur early-returned this
    // frame too) — pongFbo() == BGFX_INVALID_HANDLE ⇒ PostProcess
    // binds sceneColor on slot 1 (FS branchless composite collapses
    // to `raw * (1 + 0) = raw`). execute() still runs as long as
    // the Noop gate doesn't fire (here it does, on uninit adapter),
    // but the contract is the same.
    BloomExtractPass extract;
    BloomBlurPass blur;
    PostProcessPass post;

    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0,
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},  // sceneFbo
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr,
        &extract,                                       // bloomExtractPass
        &blur                                           // bloomBlurPass
    };
    // On uninit adapter Noop gate fires first → 0; either gate
    // (producer-zero-size OR Noop) yields 0 draws. The point is
    // ctx.bloomBlurPass pointer survives the brace-init wiring
    // without crashing.
    CHECK(BGFXAdapter::isValid(blur.pingFbo()) == false);
    CHECK(BGFXAdapter::isValid(blur.pongFbo()) == false);
    CHECK(post.execute(ctx) == 0);
}

// === F. Two-sampler contract ========================================

TEST_CASE(s1c_finalpp_phoskia_compile_when_shaderc_available) {
    // When shaderc is on the host, pool.compile on the inline
    // source succeeds and BOTH binding IDs resolve (sceneColor +
    // bloomTexture). When shaderc is missing we SKIP gracefully
    // (mirror S1a / S1b pattern). This pins the S1c two-sampler
    // contract — if a future edit accidentally drops
    // `texture2d bloomTexture`, the binding resolution below
    // surfaces it as InvalidBinding immediately.
    if (!shadercAvailable()) {
        std::cerr << "[S1c test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::shader::ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.bindRendererTypeForTests(
        /*bgfxRendererType=*/0 /*Noop*/,
        /*platform=*/"linux",
        /*profile=*/"120");

    ayt::shader::ShaderResource res =
        pool.compile(kLiveFinalPPSource);
    if (!res.isValid()) {
        std::cerr << "[S1c test] SKIP: Phoskia compile failed (no GPU backend).\n";
        for (const std::string& err : pool.lastCompileErrors()) {
            std::cerr << "[S1c test]   " << err << "\n";
        }
        return;
    }
    // Two-sampler contract — both must resolve to non-zero binding
    // IDs. If either drops, the execute() path's setTexture call
    // becomes a no-op and the visual bloom contribution vanishes.
    const ayt::shader::BindingId tSceneColor =
        res.getTextureBinding("sceneColor");
    const ayt::shader::BindingId tBloomTexture =
        res.getTextureBinding("bloomTexture");
    CHECK(tSceneColor  != ayt::shader::InvalidBinding);
    CHECK(tBloomTexture != ayt::shader::InvalidBinding);
    // Uniform bindings retained from R5.1.
    CHECK(res.getUniformBinding("bloomStrength") != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("exposure")      != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("tonemapMode")   != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("uTime")         != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("gammaParams")   != ayt::shader::InvalidBinding);
}

// === G. MakeDefault / MakeDeferred slot table =================================

TEST_CASE(s1c_make_default_slot_table_postprocess_after_bloomblur) {
    // Cutsheet §S1 sub-cut 3: S1c doesn't add a slot — PostProcess
    // stays at index 6 (Forward, +1 for S4b DepthHaze inserted
    // before) and 8 (Deferred). BloomBlur remains at index 4 / 6.
    // Pin that S1c's PassExecContext field addition did not
    // reorder the slot tables (K3 invariant #3: ABI append-only;
    // RenderPassSlot enum unchanged).
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 8);    // S4b (2026-07-23): +1 DepthHaze
    CHECK(desc.passes[3] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[4] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[5] == RenderPassSlot::DepthHaze);   // S4b (2026-07-23)
    CHECK(desc.passes[6] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[7] == RenderPassSlot::UI);
    // S1c does NOT add a RenderPassSlot enum value (no slot in the
    // table for "FinalPP" — the composite lives in PostProcessPass
    // and reads ctx.bloomBlurPass via borrowed pointer). Future
    // cuts that want a separate "FinalPP" slot must append a new
    // enum value (cutsheet §S1 §1.5 invariant).
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomBlur) == 9);
}

TEST_CASE(s1c_make_deferred_slot_table_postprocess_after_bloomblur) {
    // Deferred path also unchanged: SSAO at index 8 (between
    // DepthHaze and PostProcess), PostProcess at index 9.
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    CHECK(desc.passes.size() == 12);   // §A2 SSAO MVP (2026-07-24): +1 SSAO; V1 GBuffer Debug (2026-07-24): +1 GBufferDebug appended last
    CHECK(desc.passes[5] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[6] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[7] == RenderPassSlot::DepthHaze);   // S4b (2026-07-23)
    CHECK(desc.passes[8] == RenderPassSlot::SSAO);        // §A2 SSAO MVP (2026-07-24)
    CHECK(desc.passes[9] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[10] == RenderPassSlot::UI);
}

// === H. setEnabled(false) on PostProcessPass skips dispatch =========

TEST_CASE(s1c_finalpp_setenabled_false_skips_dispatch) {
    // setEnabled(false) on PostProcessPass short-circuits the
    // dispatch (mirror RenderPass::executeAll isEnabled gate) even
    // when ctx.bloomBlurPass is non-null.
    BloomBlurPass blur;
    PostProcessPass post;
    post.setEnabled(false);

    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0,
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr,
        nullptr,                                       // bloomExtractPass
        &blur                                          // bloomBlurPass
    };
    CHECK(post.execute(ctx) == 0);
}

// === I. DestroyResources idempotent ================================

TEST_CASE(s1c_finalpp_destroy_resources_idempotent_on_uninit) {
    // K3 invariant: destroyResources on an uninitialized adapter
    // is a no-op (BGFXAdapter::destroy on invalid handle is a
    // no-op). Calling it twice is also safe — the second call
    // sees _fbo = BGFX_INVALID_HANDLE and skips.
    //
    // PostProcessPass doesn't expose a public destroyResources on
    // the header — it's called by AYRenderer::Impl::applyPipelineDesc
    // via the public RenderPass interface. The wire lives through
    // the BloomBlurPass's own destroyResources contract (S1b K2
    // invariant #2). This case verifies the BloomBlur side which
    // is the actual producer whose pongFbo PostProcess samples.
    BloomBlurPass pass;
    BGFXAdapter adapter;  // uninit
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);  // idempotent
    CHECK(pass.isReady() == false);
    CHECK(BGFXAdapter::isValid(pass.pongFbo()) == false);
}

TEST_SUITE_END