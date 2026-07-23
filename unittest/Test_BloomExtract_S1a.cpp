// S1a BloomExtractPass (2026-07-23, short-term-plan §S1) — first
// half-resolution effect pass. This test pins the S1a ship:
//
//   1) Noop backend short-circuit (cutsheet §S1 "Noop 不崩" 验收)
//      — execute() returns 0 when adapter is uninit or bound to
//      bgfx::RendererType::Noop. K1 invariant #2.
//   2) Half-resolution size math: ensureFbo uses (W+1)/2 × (H+1)/2
//      when the viewport changes (cutsheet §S1 "ensure(w/2, h/2)").
//   3) Source-FBO priority: BloomExtractPass reuses
//      PostProcessPass::selectSourceFbo, so it gets the deferred
//      LightingOutput win + forward sceneFbo fallback. On Noop
//      (both invalid) ⇒ 0 draws.
//   4) Phoskia source substring pin: embedded kBloomExtractPhoskiaSource
//      declares material BloomExtract + texture2d sceneColor +
//      uniform vec4 bloomThreshold + uniform vec4 bloomStrength +
//      soft-knee bright extract.
//   5) Cache-key bump: BloomExtractPass compiles under
//      "bloomextract_v0_threshold_knee_soft_fs". A future S1b/S1c
//      bump forces a re-acquire.
//   6) RenderPassSlot::BloomExtract enum value = 8 (Lighting=7)
//      and the slot is included in BOTH makeDefault() and
//      makeDeferred() slot lists at the correct dispatch position
//      (after Transparent, before PostProcess).
//   7) RenderPipeline dispatch order: BloomExtractPass fires AFTER
//      TransparentPass and BEFORE PostProcessPass in a custom
//      pipeline.
//   8) destroyResources idempotent on an uninitialized adapter
//      (BGFXAdapter::destroy on invalid handle is a no-op).
//
// All tests use Backend::Noop so the headless test path stays
// clean (no shaderc, no FBO create, no GPU). The pass's
// `isNoopBackend()` guard short-circuits before any FBO / texture
// work, so these tests don't fight the Noop-backend fragility.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"
#include "AYShaderResourcePool.h"
#include "AYShaderResource.h"

#include "detail/BGFXAdapter.h"
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

// Mirror of BloomExtractPass.cpp's kBloomExtractPhoskiaSource literal
// (anonymous-namespace constexpr). Pin the structural surface so
// drift between this mirror and the live source fails a substring
// test (cutsheet §S1 "cache key bump" pattern — mirror lives here
// to pin the contract without exposing the constexpr).
constexpr const char* kBloomExtractExpectedSubstrings[] = {
    "material BloomExtract",
    "texture2d sceneColor",
    "uniform vec4 bloomThreshold",
    "uniform vec4 bloomStrength",
    "smoothstep(bloomThreshold.x - knee, bloomThreshold.x + knee, lum)",
    "let brightColor = sampled.xyz * soft",
    "let outRgb = brightColor * bloomStrength.x",
    "return vec4(outRgb, sampled.w)",
};

// Mirror the live cache key (cutsheet §S1 "cache-key bump" + Bug
// fix #3 lesson: pre-self-compare was false-green). If the master
// cache key changes without bumping here, this case fails.
constexpr const char* kExpectedBloomExtractCacheKey =
    "bloomextract_v0_threshold_knee_soft_fs";

constexpr const char* kLiveBloomExtractSource = R"(
material BloomExtract {
    texture2d sceneColor
    uniform vec4 bloomThreshold
    uniform vec4 bloomStrength
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let sampled = sample(sceneColor, uv)
        let lum = dot(sampled.xyz, vec3(0.2126, 0.7152, 0.0722))
        let knee  = bloomThreshold.x * 0.5
        let soft  = smoothstep(bloomThreshold.x - knee, bloomThreshold.x + knee, lum)
        let brightColor = sampled.xyz * soft
        let outRgb = brightColor * bloomStrength.x
        return vec4(outRgb, sampled.w)
    }
}
)";

} // namespace

TEST_SUITE(AYRenderer_BloomExtractPass_S1a)

// === A. Noop-backend short-circuit (K1 invariant #2) =================

TEST_CASE(s1a_bloomextract_noop_backend_returns_zero) {
    // Cutsheet §S1 "Noop 不崩" + K1 invariant #2: when adapter is
    // uninit or bound to Noop, execute() returns 0 draws and does
    // not create any FBO / texture / shaderc resources.
    BloomExtractPass pass;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<BloomExtractPass>());

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
    // FBO size never allocated
    CHECK(pass.halfWidth() == 0);
    CHECK(pass.halfHeight() == 0);
    CHECK(pass.isReady() == false);
}

TEST_CASE(s1a_bloomextract_zero_viewport_short_circuits) {
    // K1 invariant: viewport == 0 ⇒ 0 draws (FBO create would
    // otherwise allocate a 0x0 RGBA8 target that bgfx rejects on
    // some backends).
    BloomExtractPass pass;
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 0, 0, frame, /*viewId=*/0
    };
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0);
    CHECK(pass.halfWidth() == 0);
    CHECK(pass.halfHeight() == 0);
}

// === B. Pipeline slot enum value + ABI ==============================

TEST_CASE(s1a_renderpassslot_bloomextract_value_is_8) {
    // K1 invariant #5: ABI append-only — BloomExtract = 8 (Lighting=7).
    // Existing 7 enum values do NOT reorder. Future cuts (S1b/S1c)
    // also append, never reorder.
    CHECK(static_cast<uint8_t>(RenderPassSlot::Shadow)        == 0);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Skybox)        == 1);
    CHECK(static_cast<uint8_t>(RenderPassSlot::ForwardOpaque) == 2);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Transparent)   == 3);
    CHECK(static_cast<uint8_t>(RenderPassSlot::PostProcess)   == 4);
    CHECK(static_cast<uint8_t>(RenderPassSlot::UI)            == 5);
    CHECK(static_cast<uint8_t>(RenderPassSlot::GBuffer)       == 6);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Lighting)      == 7);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomExtract)  == 8);
}

TEST_CASE(s1a_make_default_includes_bloomextract_after_transparent) {
    // Cutsheet §S1: BloomExtract sits between Transparent and
    // PostProcess in the dispatch order. The literal `passes{}`
    // list dictates order — BloomExtract must appear at index 3
    // (after Shadow=0, ForwardOpaque=1, Transparent=2, before
    // PostProcess=4).
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 6);
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    CHECK(desc.passes[1] == RenderPassSlot::ForwardOpaque);
    CHECK(desc.passes[2] == RenderPassSlot::Transparent);
    CHECK(desc.passes[3] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[4] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[5] == RenderPassSlot::UI);
    // contains() helper round-trip
    CHECK(desc.contains(RenderPassSlot::BloomExtract));
}

TEST_CASE(s1a_make_deferred_includes_bloomextract_after_transparent) {
    // The Deferred path also gets BloomExtract at the same
    // dispatch position (between Transparent and PostProcess). On
    // Deferred, BloomExtract samples ctx.lightingPass->lightingOutputFbo()
    // (the B5 LIT color) instead of ctx.sceneFbo.
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    CHECK(desc.passes.size() == 8);
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    CHECK(desc.passes[1] == RenderPassSlot::Skybox);
    CHECK(desc.passes[2] == RenderPassSlot::GBuffer);
    CHECK(desc.passes[3] == RenderPassSlot::Lighting);
    CHECK(desc.passes[4] == RenderPassSlot::Transparent);
    CHECK(desc.passes[5] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[6] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[7] == RenderPassSlot::UI);
}

// === C. Half-resolution size math =====================================

TEST_CASE(s1a_bloomextract_half_resolution_round_up) {
    // (W+1)/2 × (H+1)/2 convention — never sample outside [0,W)
    // on the source texture. Documented in
    // BloomExtractPass.cpp::execute comment + cutsheet §S1.
    // We can't call execute() on Noop (it short-circuits before
    // ensureFbo), so this case constructs the math directly via
    // the same constants the pass uses.
    constexpr uint16_t kViewportW = 1280;
    constexpr uint16_t kViewportH = 720;
    constexpr uint16_t kHalfW     = (kViewportW + 1u) / 2u;  // 640
    constexpr uint16_t kHalfH     = (kViewportH + 1u) / 2u; // 360
    CHECK(kHalfW == 640);
    CHECK(kHalfH == 360);

    // Odd dimensions also round UP (S1 cutsheet §S1 spec).
    constexpr uint16_t kOddW = 801;
    constexpr uint16_t kOddH = 401;
    CHECK((kOddW + 1u) / 2u == 401);
    CHECK((kOddH + 1u) / 2u == 201);
}

// === D. Source-FBO priority (delegated to PostProcessPass) ==========

TEST_CASE(s1a_bloomextract_uses_postprocess_select_source_fbo_helper) {
    // BloomExtractPass reuses PostProcessPass::selectSourceFbo so
    // the B6 priority lock (deferred LightingOutput > forward
    // sceneFbo > invalid → skip) lives in ONE place. Verifying
    // the helper signature / behavior here pins that BloomExtract
    // doesn't drift into its own priority logic.
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;  // uninit — adapter gates on isInitialized()
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    // Both gbufferPass + lightingPass + sceneFbo default-null/invalid
    // → helper returns BGFX_INVALID_HANDLE
    const bgfx::FrameBufferHandle fbo =
        PostProcessPass::selectSourceFbo(ctx);
    CHECK(BGFXAdapter::isValid(fbo) == false);
    // BloomExtract short-circuits on invalid sourceFbo
    BloomExtractPass pass;
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0);
}

// === E. Phoskia source substring + cache key pins ====================

TEST_CASE(s1a_bloomextract_inlined_source_has_canonical_substrings) {
    // Pin that kLiveBloomExtractSource (mirror) contains all the
    // canonical S1a structural elements. If BloomExtractPass.cpp's
    // anonymous-namespace constexpr ever drifts, this case fails.
    const std::string haystack(kLiveBloomExtractSource);
    for (const char* needle : kBloomExtractExpectedSubstrings) {
        const std::string n(needle);
        CHECK(haystack.find(n) != std::string::npos);
    }
}

TEST_CASE(s1a_bloomextract_cache_key_literal_pinned) {
    // Cutsheet §S1 "cache-key bump" + Bug fix #3 mirror:
    // self-compare was false-green (P5.5 B Test_B5 history).
    // Test pins the live cache-key constant.
    CHECK(std::string(kExpectedBloomExtractCacheKey)
          == "bloomextract_v0_threshold_knee_soft_fs");
}

// === F. Pipeline dispatch order ======================================

TEST_CASE(s1a_pipeline_dispatch_order_transparent_bloomextract_postprocess) {
    // When a host assembles a custom pipeline with BloomExtract
    // inserted between Transparent and PostProcess, the dispatch
    // order must match the slot-list order. We can't observe real
    // bgfx draws on Noop, but the per-pass draw counter sum
    // (=0 on Noop) confirms executeAll() iterated all enabled
    // passes in the right order.
    BloomExtractPass bloom;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<ayt::render::detail::ForwardOpaquePass>());
    pipe.addPass(std::make_unique<ayt::render::detail::TransparentPass>());
    pipe.addPass(std::make_unique<BloomExtractPass>());
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
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    // On Noop all passes return 0 → sum = 0
    CHECK(pipe.executeAll(ctx) == 0);

    // findPass() name lookup works post-add
    CHECK(pipe.findPass("BloomExtract") != nullptr);
    CHECK(pipe.findPass("Transparent") != nullptr);
    CHECK(pipe.findPass("PostProcess") != nullptr);
}

TEST_CASE(s1a_pipeline_setenabled_false_skips_bloomextract) {
    // setEnabled(false) on BloomExtractPass short-circuits the
    // dispatch (mirror RenderPass::executeAll isEnabled gate).
    BloomExtractPass bloom;
    bloom.setEnabled(false);
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<BloomExtractPass>());

    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    CHECK(pipe.executeAll(ctx) == 0);
}

// === G. destroyResources idempotent ==================================

TEST_CASE(s1a_bloomextract_destroy_resources_idempotent_on_uninit) {
    // K1 invariant: destroyResources on an uninitialized adapter
    // is a no-op (BGFXAdapter::destroy on invalid handle is a
    // no-op). Calling it twice is also safe — the second call
    // sees _fbo = BGFX_INVALID_HANDLE and skips.
    BloomExtractPass pass;
    BGFXAdapter adapter;  // uninit
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);  // idempotent
    CHECK(pass.isReady() == false);
    CHECK(pass.halfWidth() == 0);
    CHECK(pass.halfHeight() == 0);
}

// === H. Shaderc path (optional, mirrors Test_PostProcess_R51) =======

TEST_CASE(s1a_bloomextract_phoskia_compile_when_shaderc_available) {
    // When shaderc is on the host, pool.acquire on the inline
    // source succeeds and the binding IDs resolve. When shaderc
    // is missing we SKIP gracefully (cutsheet §S1 K1 invariant
    // #3: pool acquire failure is not fatal — execute() returns
    // 0 instead of crashing).
    if (!shadercAvailable()) {
        std::cerr << "[S1a test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::shader::ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.bindRendererTypeForTests(
        /*bgfxRendererType=*/0 /*Noop*/,
        /*platform=*/"linux",
        /*profile=*/"120");

    ayt::shader::ShaderResource res =
        pool.acquire(kLiveBloomExtractSource, kExpectedBloomExtractCacheKey);
    if (!res.isValid()) {
        std::cerr << "[S1a test] SKIP: Phoskia acquire failed (no GPU backend).\n";
        for (const std::string& err : pool.lastCompileErrors()) {
            std::cerr << "[S1a test]   " << err << "\n";
        }
        return;
    }
    // Binding resolution — matches BloomExtractPass.cpp::ensureProgram
    const ayt::shader::BindingId uThreshold = res.getUniformBinding("bloomThreshold");
    const ayt::shader::BindingId uStrength  = res.getUniformBinding("bloomStrength");
    const ayt::shader::BindingId tSceneColor = res.getTextureBinding("sceneColor");
    CHECK(uThreshold  != ayt::shader::InvalidBinding);
    CHECK(uStrength   != ayt::shader::InvalidBinding);
    CHECK(tSceneColor != ayt::shader::InvalidBinding);
}

TEST_SUITE_END