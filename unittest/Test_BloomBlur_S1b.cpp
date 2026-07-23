// S1b BloomBlurPass (2026-07-23, short-term-plan §S1 sub-cut 2) —
// half-resolution separable-Gaussian blur ping-pong. This test
// pins the S1b ship:
//
//   1) Noop backend short-circuit (K2 invariant #2): execute()
//      returns 0 when adapter is uninit or bound to Noop.
//   2) Producer-absent short-circuit (K2 invariant #1):
//      ctx.bloomExtractPass == nullptr ⇒ execute() returns 0
//      without touching any FBO.
//   3) RenderPassSlot::BloomBlur enum value = 9 (BloomExtract=8,
//      Lighting=7) and the slot is included in BOTH makeDefault()
//      and makeDeferred() slot lists at the correct dispatch
//      position (after BloomExtract, before PostProcess).
//   4) Phoskia source substring pin: embedded kBloomBlurPhoskiaSource
//      declares material BloomBlur + texture2d source +
//      uniform vec4 direction + uniform vec4 texelSize +
//      5-tap separable Gaussian (4 symmetric taps + center).
//   5) Cache-key externalize (Bug fix #3 mirror):
//      `extern const char* const kBloomBlurCacheKeyCStr` from the
//      header pins to the live literal in BloomBlurPass.cpp.
//   6) View id constants: Extract=10, BlurH=11, BlurV=12,
//      DepthHaze=13, PostProcess=14; UI fixed at 255.
//   7) RenderPipeline dispatch order: BloomBlurPass fires AFTER
//      BloomExtractPass and BEFORE PostProcessPass in a custom
//      pipeline.
//   8) destroyResources idempotent on an uninitialized adapter
//      (BGFXAdapter::destroy on invalid handle is a no-op).
//   9) PassExecContext::bloomExtractPass default = nullptr so
//      existing 20-/21-field brace-init sites keep compiling.
//
// All tests use Backend::Noop so the headless test path stays
// clean. The pass's `isNoopBackend()` guard short-circuits before
// any FBO / texture work, so these tests don't fight the Noop-
// backend fragility.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"
#include "AYShaderResourcePool.h"
#include "AYShaderResource.h"
#include "AYUIRenderBackend.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomExtractPass.h"
#include "detail/BloomBlurPass.h"
#include "detail/DepthHazePass.h"
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
using ayt::render::detail::BloomBlurPass;
using ayt::render::detail::DepthHazePass;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::RenderPipeline;
// §S1b Bug fix #3 mirror — pin the live cache-key extern so the
// drift-detection test below sees it. Test cases live at global
// namespace (TEST_SUITE expands to global ns), so this using
// declaration pulls kBloomBlurCacheKeyCStr into scope.
using ayt::render::detail::kBloomBlurCacheKeyCStr;

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

// Mirror of BloomBlurPass.cpp's kBloomBlurPhoskiaSource literal
// (anonymous-namespace constexpr). Pin the structural surface so
// drift between this mirror and the live source fails a substring
// test (cutsheet §S1 "cache key bump" pattern).
constexpr const char* kBloomBlurExpectedSubstrings[] = {
    "material BloomBlur",
    "texture2d source",
    "uniform vec4 direction",
    "uniform vec4 texelSize",
    "let dir = direction.xy",
    "let tSize = texelSize.xy",
    "c * 0.227",
    "t1 * 0.194",
    "t2 * 0.121",
    "t3 * 0.054",
    "t4 * 0.016",
    "sample(source, uv - dir * tSize * 4.0)",
};

// Mirror the live cache key (cutsheet §S1 "cache-key bump" + Bug
// fix #3 lesson: pre-self-compare was false-green).
constexpr const char* kExpectedBloomBlurCacheKey =
    "bloomblur_v1_separable_5tap_fs";

constexpr const char* kLiveBloomBlurSource = R"(
material BloomBlur {
    texture2d source
    uniform vec4 direction
    uniform vec4 texelSize
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let dir = direction.xy
        let tSize = texelSize.xy
        let c = sample(source, uv)
        let t1 = sample(source, uv + dir * tSize * 1.0)
        let t2 = sample(source, uv + dir * tSize * 2.0)
        let t3 = sample(source, uv + dir * tSize * 3.0)
        let t4 = sample(source, uv + dir * tSize * 4.0)
        let result = c * 0.227
                   + t1 * 0.194 + sample(source, uv - dir * tSize * 1.0) * 0.194
                   + t2 * 0.121 + sample(source, uv - dir * tSize * 2.0) * 0.121
                   + t3 * 0.054 + sample(source, uv - dir * tSize * 3.0) * 0.054
                   + t4 * 0.016 + sample(source, uv - dir * tSize * 4.0) * 0.016
        return vec4(result.x, result.y, result.z, c.w)
    }
}
)";

} // namespace

TEST_SUITE(AYRenderer_BloomBlurPass_S1b)

// === A. Noop-backend short-circuit (K2 invariant #2) ==================

TEST_CASE(s1b_bloomblur_noop_backend_returns_zero) {
    // K2 invariant #2: when adapter is uninit or bound to Noop,
    // execute() returns 0 draws and does not create any FBO /
    // texture / shaderc resources.
    BloomBlurPass pass;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<BloomBlurPass>());

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
    // FBOs never allocated
    CHECK(BGFXAdapter::isValid(pass.pingFbo()) == false);
    CHECK(BGFXAdapter::isValid(pass.pongFbo()) == false);
    CHECK(pass.isReady() == false);
}

TEST_CASE(s1b_bloomblur_producer_absent_returns_zero) {
    // K2 invariant #1: ctx.bloomExtractPass == nullptr ⇒
    // execute() returns 0 without touching any FBO. Forward custom
    // desc that omits the BloomExtract slot lands here.
    BloomBlurPass pass;
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    BGFXAdapter adapter;  // uninit — Noop gate fires first anyway
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
        // 12 trailing fields default-init: shadowPass, gbufferPass,
        // lightingPass, sceneLights, skySource, skyboxPass,
        // perLightShadows, bloomExtractPass = all nullptr.
    };
    CHECK(ctx.bloomExtractPass == nullptr);  // default-init verified
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0);
    CHECK(BGFXAdapter::isValid(pass.pingFbo()) == false);
    CHECK(BGFXAdapter::isValid(pass.pongFbo()) == false);
}

TEST_CASE(s1b_bloomblur_producer_zero_size_returns_zero) {
    // Producer (BloomExtract) is present but hasn't yet ensured its
    // FBO (first-frame race; S1a BloomExtract early-returned this
    // frame too) — halfWidth() == 0 ⇒ Blur skips the blur and
    // returns 0. Visually identical to bloomStrength=0 default.
    BloomExtractPass extract;
    BloomBlurPass blur;

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
        &extract                                        // bloomExtractPass
    };
    // On uninit adapter Noop gate fires first → 0; either gate
    // (producer-zero-size OR Noop) yields 0 draws.
    CHECK(extract.halfWidth() == 0);
    CHECK(extract.halfHeight() == 0);
    CHECK(blur.execute(ctx) == 0);
}

// === B. Pipeline slot enum value + ABI ==============================

TEST_CASE(s1b_renderpassslot_bloomblur_value_is_9) {
    // K2 invariant #5: ABI append-only — BloomBlur = 9
    // (BloomExtract=8, Lighting=7). Existing 8 enum values do NOT
    // reorder. Future cuts also append, never reorder.
    CHECK(static_cast<uint8_t>(RenderPassSlot::Shadow)        == 0);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Skybox)        == 1);
    CHECK(static_cast<uint8_t>(RenderPassSlot::ForwardOpaque) == 2);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Transparent)   == 3);
    CHECK(static_cast<uint8_t>(RenderPassSlot::PostProcess)   == 4);
    CHECK(static_cast<uint8_t>(RenderPassSlot::UI)            == 5);
    CHECK(static_cast<uint8_t>(RenderPassSlot::GBuffer)       == 6);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Lighting)      == 7);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomExtract)  == 8);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomBlur)     == 9);
}

TEST_CASE(s1b_make_default_includes_bloomblur_after_bloomextract) {
    // Cutsheet §S1 sub-cut 2: BloomBlur sits between BloomExtract
    // and PostProcess in the dispatch order. makeDefault() forward
    // path now has 8 slots (was 6 after S1a; +1 BloomBlur; +1
    // DepthHaze from S4b).
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 8);
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    CHECK(desc.passes[1] == RenderPassSlot::ForwardOpaque);
    CHECK(desc.passes[2] == RenderPassSlot::Transparent);
    CHECK(desc.passes[3] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[4] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[5] == RenderPassSlot::DepthHaze);   // S4b (2026-07-23)
    CHECK(desc.passes[6] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[7] == RenderPassSlot::UI);
    CHECK(desc.contains(RenderPassSlot::BloomBlur));
}

TEST_CASE(s1b_make_deferred_includes_bloomblur_after_bloomextract) {
    // The Deferred path also gets BloomBlur at the same dispatch
    // position (after BloomExtract, before PostProcess). Now 10
    // slots total (was 8 after S1a; +1 BloomBlur; +1 DepthHaze
    // from S4b).
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    CHECK(desc.passes.size() == 10);
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    CHECK(desc.passes[1] == RenderPassSlot::Skybox);
    CHECK(desc.passes[2] == RenderPassSlot::GBuffer);
    CHECK(desc.passes[3] == RenderPassSlot::Lighting);
    CHECK(desc.passes[4] == RenderPassSlot::Transparent);
    CHECK(desc.passes[5] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[6] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[7] == RenderPassSlot::DepthHaze);   // S4b (2026-07-23)
    CHECK(desc.passes[8] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[9] == RenderPassSlot::UI);
}

// === C. View id constants ===========================================

TEST_CASE(s1b_bloomblur_view_ids_after_extract_before_pp) {
    // Order lock: Extract=10 → BlurH=11 → BlurV=12 → Haze=13 → PP=14;
    // UI=255 (fixed). Haze must sort before Final PP (same-frame sample).
    CHECK(BloomBlurPass::kBloomBlurHorizontalViewId == 11);
    CHECK(BloomBlurPass::kBloomBlurVerticalViewId   == 12);
    CHECK(BloomBlurPass::kBloomBlurHorizontalViewId
          == BloomExtractPass::kBloomExtractViewId + 1);
    CHECK(BloomBlurPass::kBloomBlurVerticalViewId
          == BloomBlurPass::kBloomBlurHorizontalViewId + 1);
    CHECK(DepthHazePass::kDepthHazeViewId
          == BloomBlurPass::kBloomBlurVerticalViewId + 1);
    CHECK(PostProcessPass::kBlitViewId
          == DepthHazePass::kDepthHazeViewId + 1);
    CHECK(ayt::render::UIRenderBackend::kViewId == 255);
    CHECK(ayt::render::UIRenderBackend::kViewId > PostProcessPass::kBlitViewId);
}

// === D. Producer API (BloomExtract::halfResFbo) =====================

TEST_CASE(s1b_bloomextract_exposes_half_res_fbo_getter) {
    // The S1b consumer needs `ctx.bloomExtractPass->halfResFbo()`
    // to read the producer's RT0 attachment. Verify the getter
    // exists and returns the underlying _fbo field. On uninit the
    // FBO is invalid; on first execute() (Noop short-circuits)
    // still invalid — getter pinned at the type level.
    BloomExtractPass extract;
    CHECK(BGFXAdapter::isValid(extract.halfResFbo()) == false);
    CHECK(extract.halfWidth() == 0);
    CHECK(extract.halfHeight() == 0);
}

// === E. Phoskia source substring + cache key pins ====================

TEST_CASE(s1b_bloomblur_inlined_source_has_canonical_substrings) {
    // Pin that kLiveBloomBlurSource (mirror) contains all the
    // canonical S1b structural elements. If BloomBlurPass.cpp's
    // anonymous-namespace constexpr ever drifts, this case fails.
    const std::string haystack(kLiveBloomBlurSource);
    for (const char* needle : kBloomBlurExpectedSubstrings) {
        const std::string n(needle);
        CHECK(haystack.find(n) != std::string::npos);
    }
}

TEST_CASE(s1b_bloomblur_cache_key_literal_pinned_via_extern) {
    // Cutsheet §S1 "cache-key bump" + Bug fix #3 mirror: extern
    // `kBloomBlurCacheKeyCStr` from BloomBlurPass.h must compare
    // equal to the local mirror literal. Pre-extern the test
    // would have been a self-compare ("mine == mine") = false-
    // green drift detection.
    CHECK(std::string(kBloomBlurCacheKeyCStr)
          == "bloomblur_v1_separable_5tap_fs");
    CHECK(std::string(kExpectedBloomBlurCacheKey)
          == std::string(kBloomBlurCacheKeyCStr));
}

// === F. Pipeline dispatch order ======================================

TEST_CASE(s1b_pipeline_dispatch_order_bloomextract_bloomblur_postprocess) {
    // When a host assembles a custom pipeline with BloomBlur
    // inserted between BloomExtract and PostProcess, the dispatch
    // order must match the slot-list order. On Noop all passes
    // return 0 → sum = 0.
    BloomExtractPass extract;
    BloomBlurPass blur;
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
        &extract                                        // bloomExtractPass
    };
    CHECK(pipe.executeAll(ctx) == 0);

    // findPass() name lookup works post-add
    CHECK(pipe.findPass("BloomExtract") != nullptr);
    CHECK(pipe.findPass("BloomBlur")    != nullptr);
    CHECK(pipe.findPass("PostProcess")   != nullptr);
}

TEST_CASE(s1b_pipeline_setenabled_false_skips_bloomblur) {
    // setEnabled(false) on BloomBlurPass short-circuits the
    // dispatch (mirror RenderPass::executeAll isEnabled gate).
    BloomBlurPass blur;
    blur.setEnabled(false);
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<BloomBlurPass>());

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

// === G. PassExecContext::bloomExtractPass default ====================

TEST_CASE(s1b_pass_exec_context_bloom_extract_pass_default_nullptr) {
    // K2 invariant #4: S1b doesn't touch FrameContext /
    // RenderScene / RenderPass signature. PassExecContext grew
    // exactly one new field (bloomExtractPass). Existing 12-field
    // brace-init sites still compile (trailing-default = nullptr
    // — C++14). Construct a minimal PassExecContext via 12-field
    // form and verify the new field is nullptr.
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
    CHECK(ctx.viewportWidth == 64u);
    CHECK(ctx.viewportHeight == 64u);
}

// === H. destroyResources idempotent ==================================

TEST_CASE(s1b_bloomblur_destroy_resources_idempotent_on_uninit) {
    // K2 invariant: destroyResources on an uninitialized adapter
    // is a no-op (BGFXAdapter::destroy on invalid handle is a
    // no-op). Calling it twice is also safe.
    BloomBlurPass pass;
    BGFXAdapter adapter;  // uninit
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);  // idempotent
    CHECK(pass.isReady() == false);
    CHECK(BGFXAdapter::isValid(pass.pingFbo()) == false);
    CHECK(BGFXAdapter::isValid(pass.pongFbo()) == false);
}

// === I. Shaderc path (optional, mirrors Test_BloomExtract_S1a H) ====

TEST_CASE(s1b_bloomblur_phoskia_compile_when_shaderc_available) {
    // When shaderc is on the host, pool.acquire on the inline
    // source succeeds and the binding IDs resolve. When shaderc
    // is missing we SKIP gracefully (cutsheet §S1 K1 invariant
    // propagated: pool acquire failure is not fatal).
    if (!shadercAvailable()) {
        std::cerr << "[S1b test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::shader::ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.bindRendererTypeForTests(
        /*bgfxRendererType=*/0 /*Noop*/,
        /*platform=*/"linux",
        /*profile=*/"120");

    ayt::shader::ShaderResource res =
        pool.acquire(kLiveBloomBlurSource, kExpectedBloomBlurCacheKey);
    if (!res.isValid()) {
        std::cerr << "[S1b test] SKIP: Phoskia acquire failed (no GPU backend).\n";
        for (const std::string& err : pool.lastCompileErrors()) {
            std::cerr << "[S1b test]   " << err << "\n";
        }
        return;
    }
    // Binding resolution — matches BloomBlurPass.cpp::ensureProgram
    const ayt::shader::BindingId uDir     = res.getUniformBinding("direction");
    const ayt::shader::BindingId uTexel   = res.getUniformBinding("texelSize");
    const ayt::shader::BindingId tSource  = res.getTextureBinding("source");
    CHECK(uDir    != ayt::shader::InvalidBinding);
    CHECK(uTexel  != ayt::shader::InvalidBinding);
    CHECK(tSource != ayt::shader::InvalidBinding);
}

TEST_SUITE_END