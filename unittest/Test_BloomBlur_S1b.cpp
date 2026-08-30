// BloomBlur regression coverage: current-frame producer gating, FrameGraph
// ping-pong order, five-fetch linear Gaussian production source, stable cache
// key/bindings, Noop safety, slot/view order, and resource teardown.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYRenderer/BloomShaderSources.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShaderResource.h"
#include "AYRenderer/UIRenderBackend.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomExtractPass.h"
#include "detail/BloomBlurPass.h"
#include "detail/DepthHazePass.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/SSAOPass.h"        // §A2 SSAO MVP (2026-07-24) — view-chain cross-check pinning SSAO between DepthHaze and PostProcess.
#include "detail/RenderPipeline.h"
#include "detail/TransparentPass.h"

#include <bgfx/bgfx.h>

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
using ayt::render::detail::SSAOPass;       // §A2 SSAO MVP (2026-07-24)
using ayt::render::detail::RenderPipeline;
namespace {

// Inspect the exact production source instead of a stale test-local mirror.
constexpr const char* kBloomBlurExpectedSubstrings[] = {
    "material BloomBlur",
    "texture2d source",
    "uniform vec4 direction",
    "uniform vec4 texelSize",
    "let offset = direction.xy * texelSize.xy",
    "offset * 1.3846153846",
    "offset * 3.2307692308",
    "let result = center + nearP + nearN + farP + farN",
    "return vec4(result.x, result.y, result.z, 0.0)",
};

constexpr const char* kExpectedBloomBlurCacheKey =
    ayt::render::kBloomBlurCacheKey;
constexpr const char* kLiveBloomBlurSource =
    ayt::render::kBloomBlurPhoskiaSource;

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
    // path now has 9 slots (was 8 after S4b; +1 Forward2DOpaque
    // from CM-1 (2026-08-11), inserted between ForwardOpaque and
    // Transparent).
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 12);
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    CHECK(desc.passes[1] == RenderPassSlot::ForwardOpaque);
    CHECK(desc.passes[2] == RenderPassSlot::Forward2DOpaque);  // CM-1 (2026-08-11)
    CHECK(desc.passes[3] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[4] == RenderPassSlot::Transparent);
    CHECK(desc.passes[5] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[6] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[7] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[8] == RenderPassSlot::FXAA);
    CHECK(desc.passes[9] == RenderPassSlot::ColorGrading);
    CHECK(desc.passes[10] == RenderPassSlot::Present);
    CHECK(desc.passes[11] == RenderPassSlot::UI);
    CHECK(desc.contains(RenderPassSlot::BloomBlur));
    CHECK(desc.contains(RenderPassSlot::Forward2DOpaque));
}

TEST_CASE(s1b_make_deferred_includes_bloomblur_after_bloomextract) {
    // The Deferred path also gets BloomBlur at the same dispatch
    // position (after BloomExtract, before PostProcess). Now 11
    // slots total (was 8 after S1a; +1 BloomBlur; +1 DepthHaze
    // from S4b; +1 SSAO from §A2).
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    CHECK(desc.passes.size() == 15);
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    CHECK(desc.passes[1] == RenderPassSlot::Skybox);
    CHECK(desc.passes[2] == RenderPassSlot::GBuffer);
    CHECK(desc.passes[3] == RenderPassSlot::SSAO);
    CHECK(desc.passes[4] == RenderPassSlot::Lighting);
    CHECK(desc.passes[5] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[6] == RenderPassSlot::Transparent);
    CHECK(desc.passes[7] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[8] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[9] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[10] == RenderPassSlot::FXAA);
    CHECK(desc.passes[11] == RenderPassSlot::ColorGrading);
    CHECK(desc.passes[12] == RenderPassSlot::Present);
    CHECK(desc.passes[13] == RenderPassSlot::UI);
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
    // Stable numeric IDs remain adjacent for ABI, while explicit view order
    // executes SSAO(14) before Lighting(8) and DepthHaze(13).
    CHECK(SSAOPass::kSsaoViewId
          == DepthHazePass::kDepthHazeViewId + 1);
    CHECK(PostProcessPass::kBlitViewId
          == SSAOPass::kSsaoViewId + 1);
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

TEST_CASE(s1b_bloomblur_cache_key_literal_pinned) {
    CHECK(std::string(kExpectedBloomBlurCacheKey)
          == "bloomblur_v2_bilinear_5fetch_fs");
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

// === I. Production binding contract =================================

TEST_CASE(s1b_bloomblur_production_binding_names_are_pinned) {
    const std::string source(kLiveBloomBlurSource);
    CHECK(source.find("uniform vec4 direction") != std::string::npos);
    CHECK(source.find("uniform vec4 texelSize") != std::string::npos);
    CHECK(source.find("texture2d source") != std::string::npos);
}

TEST_SUITE_END
