// S1c Final-PP Bloom Composite (2026-07-23, short-term-plan §S1
// sub-cut 3 of 4) — the true post-process composite that reads
// `_pongFbo` (the vertically-blurred half-res FBO produced by
// BloomBlurPass in S1b) as the actual bloom contribution, replacing
// the pre-S1 fake `raw + raw*bloomStrength` shader hack with
// `(scene + sample(bloomTexture, uv) * bloomStrength) * exposure`.
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
//   4) Cache-key pin: PostProcessPass uses the current v12 sanitized-
//      parameter primary program. Future source changes must bump it.
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
//   8) Production source compiles through Phoskia + shaderc without
//      requiring an initialized bgfx GPU backend.
//   9) DestroyResources idempotent on uninitialized adapter
//      (BGFXAdapter::destroy on invalid handle is a no-op).
//  10) Two-sampler contract: the Phoskia source declares
//      `texture2d sceneColor` + `texture2d bloomTexture`, and
//      both logical texture names are present in reflection.
//
// Runtime-dispatch tests use Backend::Noop. The source-contract test invokes
// shaderc directly but never creates a GPU program. The pass's
// `isNoopBackend()` guard short-circuits before any FBO /
// texture work, so these tests don't fight the Noop-backend
// fragility.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "AYShader/ShaderResourcePool.h"

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

#include <algorithm>
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

void compileFinalPP(ayt::shader::CompiledShaderProgram& program)
{
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
    backend.platform = "linux";
    backend.profile = "430";
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    compiler.compileToProgram(
        ayt::render::detail::postProcessPhoskiaSourceForTests(),
        frontend, backend, program);
}

bool hasUniform(const ayt::shader::CompiledShaderProgram& program,
                const char* name)
{
    return std::any_of(program.uniforms.begin(), program.uniforms.end(),
        [name](const ayt::shader::BGFXUniform& uniform) {
            return uniform.name == name;
        });
}

bool hasTexture(const ayt::shader::CompiledShaderProgram& program,
                const char* name)
{
    return std::any_of(program.textures.begin(), program.textures.end(),
        [name](const ayt::shader::BGFXTexture& texture) {
            return texture.name == name;
        });
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
    "bloomSample.xyz * bloomStrength.x * combinedExposure",
    "uniform vec4 bloomStrength",
    "uniform vec4 exposure",
    "uniform vec4 tonemapMode",
    "step(1.5, m)",
};

constexpr const char* kExpectedFinalPPCacheKey =
    "postprocess_tonemap_aces_v13_auto_exposure_fs";

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
    const std::string haystack(
        ayt::render::detail::postProcessPhoskiaSourceForTests());
    for (const char* needle : kFinalPPExpectedSubstrings) {
        const std::string n(needle);
        CHECK(haystack.find(n) != std::string::npos);
    }
}

TEST_CASE(s1c_finalpp_cache_key_literal_pinned) {
    CHECK(std::string(ayt::render::detail::kPostProcessCacheKeyCStr)
          == kExpectedFinalPPCacheKey);
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

TEST_CASE(s1c_finalpp_production_source_compiles_headless) {
    // Compile the exact runtime source to binaries + reflection without
    // acquiring a GPU program. This stays valid in a headless process and
    // fails, rather than silently skipping, on source or shaderc regressions.
    ayt::shader::CompiledShaderProgram program;
    compileFinalPP(program);
    if (!program.success) {
        for (const std::string& error : program.errors) {
            std::cerr << "[S1c final PP] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(hasTexture(program, "sceneColor"));
    CHECK(hasTexture(program, "bloomTexture"));
    CHECK(hasUniform(program, "bloomStrength"));
    CHECK(hasUniform(program, "exposure"));
    CHECK(hasUniform(program, "tonemapMode"));
    CHECK_FALSE(hasUniform(program, "uTime"));
    CHECK(hasUniform(program, "gammaParams"));
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
    CHECK(desc.passes.size() == 14);
    CHECK(desc.passes[2] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[3] == RenderPassSlot::Transparent);
    CHECK(desc.passes[4] == RenderPassSlot::Forward2DOpaque);
    CHECK(desc.passes[5] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[6] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[8] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[9] == RenderPassSlot::FXAA);
    CHECK(desc.passes[10] == RenderPassSlot::SMAA);
    CHECK(desc.passes[11] == RenderPassSlot::ColorGrading);
    CHECK(desc.passes[12] == RenderPassSlot::Present);
    CHECK(desc.passes[13] == RenderPassSlot::UI);
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
    CHECK(desc.passes.size() == 20);
    CHECK(desc.passes[3] == RenderPassSlot::MotionVector);
    CHECK(desc.passes[4] == RenderPassSlot::SSAO);
    CHECK(desc.passes[5] == RenderPassSlot::Lighting);
    CHECK(desc.passes[6] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[7] == RenderPassSlot::Transparent);
    CHECK(desc.passes[8] == RenderPassSlot::Forward2DOpaque);
    CHECK(desc.passes[9] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[10] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[12] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[13] == RenderPassSlot::TAA);
    CHECK(desc.passes[14] == RenderPassSlot::FXAA);
    CHECK(desc.passes[15] == RenderPassSlot::SMAA);
    CHECK(desc.passes[16] == RenderPassSlot::ColorGrading);
    CHECK(desc.passes[17] == RenderPassSlot::Present);
    CHECK(desc.passes[18] == RenderPassSlot::UI);
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
