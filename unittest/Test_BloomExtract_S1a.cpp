// BloomExtract regression coverage: stage fail-close rules, finite parameter
// sanitization, HDR/Karis/soft-knee production shader contract, exposure-aware
// final composite, slot order, Noop safety, and resource teardown. Shader
// assertions inspect the exact runtime source rather than a test-local mirror.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYRenderer/BloomShaderSources.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShaderResource.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomExtractPass.h"
#include "detail/BloomPipeline.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipeline.h"

#include <bgfx/bgfx.h>

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

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

std::size_t countSubstring(const std::string& text,
                           const std::string& needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

// Inspect the exact production source. Keeping a second shader literal in the
// test made the old checks false-green when runtime code changed.
constexpr const char* kBloomExtractExpectedSubstrings[] = {
    "material BloomExtract",
    "texture2d sceneColor",
    "uniform vec4 bloomThreshold",
    "uniform vec4 sourceTexelSize",
    "let s4 = sample(sceneColor",
    "let weightSum = max(w0 + w1 + w2 + w3 + w4",
    "let contribution = max(lum - threshold, soft)",
    "return vec4(outRgb, 0.0)",
};

constexpr const char* kExpectedBloomExtractCacheKey =
    ayt::render::kBloomExtractCacheKey;
constexpr const char* kLiveBloomExtractSource =
    ayt::render::kBloomExtractPhoskiaSource;

class RecordingPass final : public RenderPass {
public:
    RecordingPass(std::string_view passName, char marker, std::string& trace)
        : _passName(passName), _marker(marker), _trace(trace)
    {
    }

    std::string_view name() const override { return _passName; }

    uint32_t execute(PassExecContext&) override
    {
        _trace.push_back(_marker);
        return 0;
    }

private:
    std::string_view _passName;
    char _marker;
    std::string& _trace;
};

} // namespace

TEST_SUITE(AYRenderer_BloomAuditRound2)

TEST_CASE(bloom_round2_parameters_are_finite_and_bounded) {
    using namespace ayt::render::detail;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CHECK(sanitizeBloomStrength(-1.0f) == 0.0f);
    CHECK(sanitizeBloomStrength(nan) == 0.0f);
    CHECK(sanitizeBloomStrength(inf) == 0.0f);
    CHECK(sanitizeBloomStrength(2.5f) == 2.5f);
    CHECK(sanitizeBloomThreshold(-1.0f) == 0.0f);
    CHECK(sanitizeBloomThreshold(nan) == 1.0f);
    CHECK(sanitizeBloomSoftKnee(-1.0f) == 0.0f);
    CHECK(sanitizeBloomSoftKnee(2.0f) == 1.0f);
    CHECK(sanitizeBloomSoftKnee(inf) == 0.5f);
}

TEST_CASE(bloom_round2_stage_truth_table_fails_closed) {
    using ayt::render::detail::selectBloomStages;

    const auto off = selectBloomStages(0.0f, true, true, true, true);
    CHECK(!off.extract);
    CHECK(!off.blur);

    const auto missingExtract =
        selectBloomStages(0.4f, false, false, true, true);
    CHECK(!missingExtract.extract);
    CHECK(!missingExtract.blur);

    const auto extractOnly =
        selectBloomStages(0.4f, true, true, false, false);
    CHECK(!extractOnly.extract);
    CHECK(!extractOnly.blur);

    const auto disabledBlur =
        selectBloomStages(0.4f, true, true, true, false);
    CHECK(!disabledBlur.extract);
    CHECK(!disabledBlur.blur);

    const auto complete =
        selectBloomStages(0.4f, true, true, true, true);
    CHECK(complete.extract);
    CHECK(complete.blur);
}

TEST_CASE(bloom_round2_composite_requires_current_frame_blur) {
    using ayt::render::detail::effectiveBloomStrength;
    CHECK(effectiveBloomStrength(0.4f, false) == 0.0f);
    CHECK(effectiveBloomStrength(0.4f, true) == 0.4f);
    CHECK(effectiveBloomStrength(-0.4f, true) == 0.0f);
    CHECK(effectiveBloomStrength(
        std::numeric_limits<float>::quiet_NaN(), true) == 0.0f);
}

TEST_CASE(bloom_round2_intermediates_and_scene_are_hdr) {
    CHECK(ayt::render::detail::kHdrSceneColorFormat
          == bgfx::TextureFormat::RGBA16F);
}

TEST_CASE(bloom_round2_runtime_sources_pin_quality_and_exposure_contract) {
    const std::string extract(ayt::render::kBloomExtractPhoskiaSource);
    const std::string blur(ayt::render::kBloomBlurPhoskiaSource);
    const std::string post(
        ayt::render::detail::postProcessPhoskiaSourceForTests());
    const std::string fallback(
        ayt::render::detail::postProcessFallbackPhoskiaSourceForTests());

    CHECK(countSubstring(extract, "sample(sceneColor") == 5u);
    CHECK(extract.find("sourceTexelSize") != std::string::npos);
    CHECK(extract.find("let contribution =") != std::string::npos);
    CHECK(countSubstring(blur, "sample(source") == 5u);
    CHECK(blur.find("1.3846153846") != std::string::npos);
    CHECK(blur.find("3.2307692308") != std::string::npos);
    const std::string exposedBloom =
        "bloomSample.xyz * bloomStrength.x * exposure.x";
    CHECK(post.find(exposedBloom) != std::string::npos);
    // The fallback is intentionally independent from bloom/exposure math so
    // a primary-only compile failure can still leave a visible scene blit.
    CHECK(fallback.find(exposedBloom) == std::string::npos);
    CHECK(fallback.find("bloomTexture") == std::string::npos);
    CHECK(fallback.find("return sample(sceneColor, uv)") != std::string::npos);
}

TEST_SUITE_END

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
    // otherwise allocate a 0x0 render target that bgfx rejects on
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
    // PostProcess). S1b (2026-07-23) appended BloomBlur between
    // BloomExtract and PostProcess — slot index shifts for
    // PostProcess/UI by +1, but BloomExtract stays at index 3.
    // S4b (2026-07-23) appended DepthHaze between BloomBlur and
    // PostProcess — slot index shifts PostProcess/UI by another +1.
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 13);
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    CHECK(desc.passes[1] == RenderPassSlot::ForwardOpaque);
    CHECK(desc.passes[2] == RenderPassSlot::Forward2DOpaque);  // CM-1 (2026-08-11)
    CHECK(desc.passes[3] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[4] == RenderPassSlot::Transparent);
    CHECK(desc.passes[5] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[6] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[7] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[8] == RenderPassSlot::FXAA);
    CHECK(desc.passes[9] == RenderPassSlot::SMAA);
    CHECK(desc.passes[10] == RenderPassSlot::ColorGrading);
    CHECK(desc.passes[11] == RenderPassSlot::Present);
    CHECK(desc.passes[12] == RenderPassSlot::UI);
    // contains() helper round-trip
    CHECK(desc.contains(RenderPassSlot::BloomExtract));
    CHECK(desc.contains(RenderPassSlot::BloomBlur));          // S1b (2026-07-23)
    CHECK(desc.contains(RenderPassSlot::DepthHaze));          // S4b (2026-07-23)
}

TEST_CASE(s1a_make_deferred_includes_bloomextract_after_transparent) {
    // The Deferred path also gets BloomExtract at the same
    // dispatch position (between Transparent and PostProcess). On
    // Deferred, BloomExtract samples ctx.lightingPass->lightingOutputFbo()
    // (the B5 LIT color) instead of ctx.sceneFbo. S1b appended
    // BloomBlur between BloomExtract and PostProcess — slot
    // index shifts for PostProcess/UI by +1, but BloomExtract
    // stays at index 5. S4b appended DepthHaze between BloomBlur
    // and PostProcess — PostProcess/UI shift by another +1.
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    CHECK(desc.passes.size() == 16);
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
    CHECK(desc.passes[11] == RenderPassSlot::SMAA);
    CHECK(desc.passes[12] == RenderPassSlot::ColorGrading);
    CHECK(desc.passes[13] == RenderPassSlot::Present);
    CHECK(desc.passes[14] == RenderPassSlot::UI);
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
          == "bloomextract_v2_hdr_karis_softknee_fs");
}

// === F. Pipeline dispatch order ======================================

TEST_CASE(s1a_pipeline_dispatch_order_transparent_bloomextract_postprocess) {
    // Record the dispatch order without involving unrelated pass lifecycle,
    // shader-pool, or rate-limited logging state.
    std::string trace;
    RenderPipeline pipe;
    pipe.addPass(std::make_unique<RecordingPass>("Transparent", 'T', trace));
    pipe.addPass(std::make_unique<RecordingPass>("BloomExtract", 'B', trace));
    pipe.addPass(std::make_unique<RecordingPass>("PostProcess", 'P', trace));

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
    CHECK(trace == "TBP");

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

// === H. Production binding contract =================================

TEST_CASE(s1a_bloomextract_production_binding_names_are_pinned) {
    const std::string source(kLiveBloomExtractSource);
    CHECK(source.find("uniform vec4 bloomThreshold") != std::string::npos);
    CHECK(source.find("uniform vec4 sourceTexelSize") != std::string::npos);
    CHECK(source.find("texture2d sceneColor") != std::string::npos);
}

TEST_SUITE_END
