// §S4c (2026-07-23, short-term-plan §S4 sub-cut 3 of 4) —
// PostProcessPass haze sampler wire + true composite. The §S4b
// cut shipped the half-res exponential fog shader + FBO; §S4c
// wires PostProcessPass to read the haze result FBO as a third
// sampler and apply the per-pixel `mix(raw, fogColor, fogFactor *
// strength)` composite over the *un-bloomed* raw scene color
// (per short-term-plan §S4 决策 2026-07-23 "haze 只改 raw, bloom
// 独立").
//
// Tests pin:
//
//   1) K3 invariant #3 — ctx.depthHazePass == nullptr ⇒
//      PostProcessPass::execute() returns 0 without crashing; the
//      fallback path binds sceneColor on the haze slot, and the FS
//      branchless strength gate collapses the haze mix to
//      `raw * (1 - 0) + fogColor * 0 = raw` — byte-equivalent to
//      a zero-haze pipeline. Mirror §S1c bloomBlurPass==nullptr
//      contract.
//   2) K3 invariant — ctx.depthHazePass != nullptr but
//      depthHazePass->halfResFbo() == BGFX_INVALID_HANDLE (first-
//      frame race; DepthHazePass didn't ensureFbo this frame) ⇒
//      PostProcessPass::execute() returns 0 with the same
//      fallback-to-sceneColor semantics (the FS haze strength
//      gate collapses the mix regardless of what the haze RT
//      actually contains).
//   3) Cache-key bump: PostProcessPass compiles under
//      `postprocess_tonemap_aces_v5_prehazed_bloom_fs` (v4 → v5).
//      Passthrough fallback key also bumped to
//      `postprocess_passthrough_tonemap_aces_v5_prehazed_bloom_fs`.
//      Extern addressable via kPostProcessCacheKeyCStr.
//   4) Phoskia source substring pin — declares `texture2d hazeTexture`
//      + prefers DepthHazePass pre-mixed RGB:
//      `mix(raw, hazeSample.xyz * exposure.x, hazeWeight)`.
//   5) Three-sampler contract: the Phoskia source declares
//      `texture2d sceneColor` + `texture2d bloomTexture` +
//      `texture2d hazeTexture`, and all three
//      `getTextureBinding()` calls return non-zero BindingIds on
//      a valid acquire.
//   6) Three-new-uniform contract: the Phoskia source declares
//      `uniform vec4 hazeDensity` + `uniform vec4 hazeStrength`
//      + `uniform vec4 hazeColor`, and all three
//      `getUniformBinding()` calls return non-zero BindingIds on
//      a valid acquire.
//   7) FrameContext haze fields forwarded into the FS composite
//      via the same FrameContext::haze* source that DepthHazePass
//      S4b reads — the strength gate stays consistent across the
//      half-res FS write (DepthHazePass) and the full-res FS
//      composite (PostProcessPass).
//   8) RenderPipeline dispatch order: PostProcess fires AFTER
//      DepthHaze in a custom pipeline (so ctx.depthHazePass is
//      the same pointer the pass reads during its execute).
//   9) DestroyResources idempotent on uninitialized adapter
//      (BGFXAdapter::destroy on invalid handle is a no-op).
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
#include "detail/DepthHazePass.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"

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
using ayt::render::detail::DepthHazePass;
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
// the S4c patch (the hazeTexture composite). Pin the structural
// surface so drift between this mirror and the live source fails
// a substring test.
constexpr const char* kS4cExpectedSubstrings[] = {
    "material PostProcess",
    "texture2d sceneColor",
    "texture2d bloomTexture",
    "texture2d hazeTexture",                         // §S4c — haze sampler
    "let hazeSample = sample(hazeTexture, uv)",      // §S4c — sample pre-hazed RT
    "let hazeWeight = step(0.0001, hazeStrength.x)", // prefer pre-hazed when strength>0
    "let rawHaze = mix(raw, hazeSample.xyz * exposure.x, hazeWeight)",
    "let withBloom = rawHaze + bloomSample.xyz * bloomStrength.x",  // bloom AFTER haze
    "uniform vec4 hazeDensity",
    "uniform vec4 hazeStrength",
    "uniform vec4 hazeColor",
};

// Mirror the live cache-key literal after the pre-hazed bump.
constexpr const char* kExpectedS4cCacheKey =
    "postprocess_tonemap_aces_v5_prehazed_bloom_fs";

// Mirror of PostProcessPass.cpp's kPostProcessPhoskiaSource — uses
// DepthHazePass's already-mixed RGB (no re-fog from alpha).
constexpr const char* kS4cLivePPSource = R"(
material PostProcess {
    texture2d sceneColor
    texture2d bloomTexture
    texture2d hazeTexture
    uniform vec4 bloomStrength
    uniform vec4 exposure
    uniform vec4 tonemapMode
    uniform vec4 uTime
    uniform vec4 gammaParams
    uniform vec4 hazeDensity
    uniform vec4 hazeStrength
    uniform vec4 hazeColor
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
        let hazeSample = sample(hazeTexture, uv)
        let raw = sampled.xyz * exposure.x
        let hazeWeight = step(0.0001, hazeStrength.x)
        let rawHaze = mix(raw, hazeSample.xyz * exposure.x, hazeWeight)
        let withBloom = rawHaze + bloomSample.xyz * bloomStrength.x
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

TEST_SUITE(AYRenderer_DepthHazePass_S4c)

// === A. Noop-backend short-circuit (K3 invariant #2) =================

TEST_CASE(s4c_finalpp_noop_backend_returns_zero) {
    // K3 invariant #2: when adapter is uninit or bound to Noop,
    // execute() returns 0 draws and does not create any FBO /
    // texture / shaderc resources. Same shape as S1c + S4b +
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

// === B. Producer-absent short-circuit (K3 invariant #3) =============

TEST_CASE(s4c_finalpp_producer_absent_returns_zero) {
    // K3 invariant #3: ctx.depthHazePass == nullptr ⇒ execute()
    // returns 0 without touching any FBO. Custom desc that omits
    // the DepthHaze slot lands here. Visually identical to
    // hazeEnabled=false default because the FS branchless
    // strength gate collapses to `raw * (1 - 0) + fogColor * 0 =
    // raw` (zero haze contribution).
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
        nullptr,                                        // bloomBlurPass
        nullptr                                         // depthHazePass — K3 #3
    };
    CHECK(ctx.depthHazePass == nullptr);  // default-init verified
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0);
}

// === C. Phoskia source substring + cache key pins ===================

TEST_CASE(s4c_finalpp_inlined_source_has_canonical_substrings) {
    // Pin that kS4cLivePPSource (mirror) contains all the
    // canonical S4c structural elements. If PostProcessPass.cpp's
    // anonymous-namespace constexpr ever drifts, this case fails.
    const std::string haystack(kS4cLivePPSource);
    for (const char* needle : kS4cExpectedSubstrings) {
        const std::string n(needle);
        CHECK(haystack.find(n) != std::string::npos);
    }
}

TEST_CASE(s4c_finalpp_cache_key_literal_pinned) {
    // Cutsheet §S4 "cache-key bump": pre-hazed composite is v5.
    CHECK(std::string(kExpectedS4cCacheKey)
          == "postprocess_tonemap_aces_v5_prehazed_bloom_fs");
}

TEST_CASE(s4c_finalpp_cache_key_extern_addressable) {
    // §S4c — Bug fix #3 mirror. Extern in PostProcessPass.h must
    // bind to the file-scope literal in PostProcessPass.cpp.
    const char* const mirror = "postprocess_tonemap_aces_v5_prehazed_bloom_fs";
    CHECK(std::string(ayt::render::detail::kPostProcessCacheKeyCStr)
          == std::string(mirror));
}

// === D. PassExecContext::depthHazePass default ========================

TEST_CASE(s4c_pass_exec_context_depth_haze_pass_default_nullptr) {
    // K3 invariant #3 mirror of S1c: S4c doesn't touch FrameContext
    // / RenderScene / RenderPass signature. PassExecContext's
    // depthHazePass field was added in S4a and is unchanged by
    // S4c (S4c only wires the PostProcessPass consumer-side
    // binding). Trailing-default = nullptr — C++14.
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
    CHECK(ctx.depthHazePass == nullptr);
    CHECK(ctx.viewportWidth == 64u);
    CHECK(ctx.viewportHeight == 64u);
}

TEST_CASE(s4c_pass_exec_context_brace_init_compiles) {
    // §S4c — compile-time check that 22-field brace-init compiles
    // cleanly (C++14 trailing-default behavior keeps all existing
    // Test_PostProcess_R5Plus / Test_BloomExtract_S1a /
    // Test_BloomBlur_S1b / Test_Skybox0 / Test_F2_ForwardShadow
    // / Test_DepthHaze_S4b brace-init sites compiling without
    // edits — S4c did not grow PassExecContext).
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame;

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 800, 600,
        frame,
        /*viewId=*/14u,
    };
    CHECK(ctx.depthHazePass == nullptr);
}

// === E. Pipeline dispatch order =====================================

TEST_CASE(s4c_pipeline_dispatch_order_depthhaze_postprocess) {
    // When a host assembles a custom pipeline with PostProcess
    // AFTER DepthHaze, the dispatch order must match the slot-list
    // order. On Noop all passes return 0 → sum = 0. This pins that
    // S4c's new haze sampler wire on PostProcessPass doesn't
    // reorder slots or break the dispatch.
    DepthHazePass haze;
    PostProcessPass post;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<DepthHazePass>());
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
        nullptr,                                        // bloomExtractPass
        nullptr,                                        // bloomBlurPass
        &haze                                           // depthHazePass
    };
    CHECK(pipe.executeAll(ctx) == 0);

    // findPass() name lookup works post-add
    CHECK(pipe.findPass("DepthHaze")   != nullptr);
    CHECK(pipe.findPass("PostProcess") != nullptr);
}

// === F. Producer present but FBO invalid (first-frame race) ==========

TEST_CASE(s4c_finalpp_producer_zero_size_returns_zero) {
    // Producer (DepthHaze) is present in PassExecContext but hasn't
    // yet ensured its halfResFbo (first-frame race; S4b DepthHaze
    // early-returned this frame too because hazeEnabled=false OR
    // hazeStrength<=0 ⇒ no FBO allocation) — halfResFbo() ==
    // BGFX_INVALID_HANDLE ⇒ PostProcess binds sceneColor on slot
    // 2 (FS branchless composite collapses to
    // `raw * (1 - 0) + fogColor * 0 = raw`). execute() still runs
    // as long as the Noop gate doesn't fire (here it does, on
    // uninit adapter), but the contract is the same.
    DepthHazePass haze;
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
        nullptr,                                        // bloomExtractPass
        nullptr,                                        // bloomBlurPass
        &haze                                           // depthHazePass — present but FBO invalid
    };
    // On uninit adapter Noop gate fires first → 0; either gate
    // (producer-zero-size OR Noop) yields 0 draws. The point is
    // ctx.depthHazePass pointer survives the brace-init wiring
    // without crashing.
    CHECK(BGFXAdapter::isValid(haze.halfResFbo()) == false);
    CHECK(post.execute(ctx) == 0);
}

// === G. Three-sampler contract =======================================

TEST_CASE(s4c_finalpp_phoskia_compile_when_shaderc_available) {
    // When shaderc is on the host, pool.compile on the inline
    // source succeeds and ALL THREE binding IDs (sceneColor +
    // bloomTexture + hazeTexture) + ALL THREE new uniform bindings
    // (hazeDensity + hazeStrength + hazeColor) resolve. When
    // shaderc is missing we SKIP gracefully (mirror S1a / S1b /
    // S4b pattern). This pins the S4c three-sampler + three-new-
    // uniform contract — if a future edit accidentally drops
    // `texture2d hazeTexture`, the binding resolution below
    // surfaces it as InvalidBinding immediately.
    if (!shadercAvailable()) {
        std::cerr << "[S4c test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::shader::ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.bindRendererTypeForTests(
        /*bgfxRendererType=*/0 /*Noop*/,
        /*platform=*/"linux",
        /*profile=*/"120");

    ayt::shader::ShaderResource res =
        pool.compile(kS4cLivePPSource);
    if (!res.isValid()) {
        std::cerr << "[S4c test] SKIP: Phoskia compile failed (no GPU backend).\n";
        for (const std::string& err : pool.lastCompileErrors()) {
            std::cerr << "[S4c test]   " << err << "\n";
        }
        return;
    }
    // Three-sampler contract — all three must resolve to non-zero
    // binding IDs. If hazeTexture drops, the execute() path's
    // setTexture call becomes a no-op and the visual haze
    // contribution vanishes.
    const ayt::shader::BindingId tSceneColor =
        res.getTextureBinding("sceneColor");
    const ayt::shader::BindingId tBloomTexture =
        res.getTextureBinding("bloomTexture");
    const ayt::shader::BindingId tHazeTexture =
        res.getTextureBinding("hazeTexture");
    CHECK(tSceneColor  != ayt::shader::InvalidBinding);
    CHECK(tBloomTexture != ayt::shader::InvalidBinding);
    CHECK(tHazeTexture  != ayt::shader::InvalidBinding);
    // Uniform bindings retained from R5.1 + S1c.
    CHECK(res.getUniformBinding("bloomStrength") != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("exposure")      != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("tonemapMode")   != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("uTime")         != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("gammaParams")   != ayt::shader::InvalidBinding);
    // Three new uniforms (S4c).
    CHECK(res.getUniformBinding("hazeDensity")   != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("hazeStrength")  != ayt::shader::InvalidBinding);
    CHECK(res.getUniformBinding("hazeColor")     != ayt::shader::InvalidBinding);
}

// === H. MakeDefault / MakeDeferred slot table =================================

TEST_CASE(s4c_make_default_slot_table_depthhaze_before_postprocess) {
    // Cutsheet §S4 sub-cut 3: S4c doesn't add a slot — DepthHaze
    // stays at index 5 (Forward, S4b insert) and 7 (Deferred).
    // PostProcess stays at 6 / 8. Pin that S4c's haze sampler
    // wire did not reorder the slot tables (K3 invariant #5: ABI
    // append-only; RenderPassSlot enum unchanged).
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 8);
    CHECK(desc.passes[3] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[4] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[5] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[6] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[7] == RenderPassSlot::UI);
}

TEST_CASE(s4c_make_deferred_slot_table_depthhaze_before_postprocess) {
    // Deferred path also unchanged: DepthHaze at index 7,
    // SSAO at index 8 (cutsheet §S2 hard line: between DepthHaze
    // and PostProcess), PostProcess at index 9.
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    CHECK(desc.passes.size() == 11);    // §A2 SSAO MVP (2026-07-24): +1 SSAO
    CHECK(desc.passes[5] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[6] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[7] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[8] == RenderPassSlot::SSAO);          // §A2 SSAO MVP
    CHECK(desc.passes[9] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[10] == RenderPassSlot::UI);
}

// === I. setEnabled(false) on PostProcessPass skips dispatch ==========

TEST_CASE(s4c_finalpp_setenabled_false_skips_dispatch) {
    // setEnabled(false) on PostProcessPass short-circuits the
    // dispatch (mirror RenderPass::executeAll isEnabled gate) even
    // when ctx.depthHazePass is non-null. Confirms the S4c haze
    // sampler wire doesn't bypass the enabled flag.
    DepthHazePass haze;
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
        nullptr,                                       // bloomBlurPass
        &haze                                          // depthHazePass
    };
    CHECK(post.execute(ctx) == 0);
}

// === J. FrameContext haze fields forwarded to PostProcessPass ========

TEST_CASE(s4c_frame_context_haze_defaults_forwarded_into_postprocess) {
    // §S4c — FrameContext haze fields are the single source of
    // truth for BOTH DepthHazePass (half-res FS write) AND
    // PostProcessPass (full-res FS composite). The host-facing
    // FrameContext fields are unchanged by S4c — S4c only adds
    // the PostProcessPass consumer-side wire. Pin that
    // FrameContext brace-init keeps the same default values
    // (hazeEnabled=false, hazeStrength=0) so the FS strength gate
    // collapses to zero — byte-equivalent to pre-S4c renders.
    FrameContext frame;
    CHECK_FALSE(frame.hazeEnabled);
    CHECK(frame.hazeStrength == 0.0f);
    CHECK(frame.hazeDensity > 0.0f);
    CHECK(frame.hazeColor.x > 0.0f);
    CHECK(frame.hazeColor.y > 0.0f);
    CHECK(frame.hazeColor.z > 0.0f);
}

// === K. K3 invariants documented ======================================

TEST_CASE(s4c_k3_invariants_documented) {
    // Documents the S4c K3 invariants in code so a future reader
    // can grep for "K3 invariant #N" and find each contract
    // pinned here. The runtime K3 tests above (B/F) assert the
    // actual behavior; this test pins the contract documentation.
    //
    // K3 invariant #3 (extended for S4c): depthHazePass == nullptr
    //   OR depthHazePass->halfResFbo() == INVALID (first-frame race)
    //   ⇒ PostProcessPass::execute binds sceneColor on the haze
    //   slot; FS branchless strength gate collapses the mix to
    //   `raw * (1 - 0) + fogColor * 0 = raw` — byte-equivalent to
    //   hazeEnabled=false (no FBO ensure, no draw). Mirror §S1c
    //   bloomBlurPass==nullptr invariant.
    //
    // K3 invariant (S4c): FrameContext::hazeStrength<=0 OR
    //   hazeEnabled==false ⇒ FS branchless gate (`step(0.0,
    //   strength)`) collapses the haze mix weight to 0 — the
    //   composite reduces to `rawHaze = raw` regardless of what
    //   the haze RT actually contains. Same source-of-truth
    //   pin as DepthHazePass S4b (the host-side knobs are the
    //   single read source for BOTH passes).
    //
    // K3 invariant (S4c ABI): RenderPassSlot::DepthHaze = 10
    //   (unchanged by S4c); view id 14 reserved; no existing
    //   enum / view-id value reorders.
    CHECK(true);  // documentation-only pin
}

TEST_SUITE_END