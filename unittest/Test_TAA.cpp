#include "AYTest.h"

#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShadercDriver.h"

#include "detail/BGFXAdapter.h"
#include "detail/ColorGradingPass.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/FXAAPass.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/PresentPass.h"
#include "detail/RenderViewOrder.h"
#include "detail/SMAAPass.h"
#include "detail/TAAPass.h"
#include "detail/MotionVectorPass.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unordered_map>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::detail::FgResourceId;

std::size_t passIndex(const RenderPipelineDesc& desc, RenderPassSlot slot)
{
    const auto it = std::find(desc.passes.begin(), desc.passes.end(), slot);
    return it == desc.passes.end()
        ? desc.passes.size()
        : static_cast<std::size_t>(it - desc.passes.begin());
}

bool taaFileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

float projectedNdcComponent(const ayt::math::Float4x4& projection,
                            int component,
                            float x,
                            float y,
                            float z)
{
    const float values[4] = {x, y, z, 1.0f};
    float clipComponent = 0.0f;
    float clipW = 0.0f;
    for (int col = 0; col < 4; ++col) {
        clipComponent += projection(component, col) * values[col];
        clipW += projection(3, col) * values[col];
    }
    return clipComponent / clipW;
}

} // namespace

TEST_SUITE(AYRenderer_TAA)

TEST_CASE(taa_append_only_abi_resource_and_view_are_locked)
{
    CHECK(static_cast<uint8_t>(RenderPassSlot::TAA) == 19u);
    CHECK(static_cast<uint8_t>(FgResourceId::TaaColor) == 12u);
    CHECK(static_cast<uint8_t>(FgResourceId::Count) == 19u);
    CHECK(ayt::render::detail::TAAPass::kTaaViewId == 5u);
    CHECK(std::is_final_v<ayt::render::detail::TAAPass>);
}

TEST_CASE(taa_is_deferred_only_and_sits_between_postprocess_and_spatial_aa)
{
    CHECK_FALSE(RenderPipelineDesc::makeDefault().contains(RenderPassSlot::TAA));
    CHECK_FALSE(
        RenderPipelineDesc::makeEditorForward().contains(RenderPassSlot::TAA));
    for (const RenderPipelineDesc desc : {
             RenderPipelineDesc::makeDeferred(),
             RenderPipelineDesc::makeEditorDeferred()}) {
        const std::size_t post = passIndex(desc, RenderPassSlot::PostProcess);
        const std::size_t taa = passIndex(desc, RenderPassSlot::TAA);
        const std::size_t fxaa = passIndex(desc, RenderPassSlot::FXAA);
        const std::size_t smaa = passIndex(desc, RenderPassSlot::SMAA);
        const std::size_t grading =
            passIndex(desc, RenderPassSlot::ColorGrading);
        const std::size_t present = passIndex(desc, RenderPassSlot::Present);
        CHECK(taa == post + 1u);
        CHECK(fxaa == taa + 1u);
        CHECK(smaa == fxaa + 1u);
        CHECK(grading == smaa + 1u);
        CHECK(present == grading + 1u);
    }
}

TEST_CASE(taa_view_executes_after_postprocess_before_spatial_aa)
{
    const auto& order = ayt::render::detail::kRenderViewOrder;
    const auto post = std::find(order.begin(), order.end(),
        ayt::render::detail::PostProcessPass::kBlitViewId);
    const auto taa = std::find(order.begin(), order.end(),
        ayt::render::detail::TAAPass::kTaaViewId);
    const auto fxaa = std::find(order.begin(), order.end(),
        ayt::render::detail::FXAAPass::kFxaaViewId);
    const auto smaa = std::find(order.begin(), order.end(),
        ayt::render::detail::SMAAPass::kEdgeViewId);
    CHECK(post != order.end());
    CHECK(taa == post + 1);
    CHECK(fxaa == taa + 1);
    CHECK(smaa == fxaa + 1);
    const auto present = std::find(order.begin(), order.end(),
        ayt::render::detail::PresentPass::kPresentViewId);
    const auto debug = std::find(order.begin(), order.end(),
        ayt::render::detail::TAAPass::kDebugViewId);
    CHECK(present != order.end());
    CHECK(debug == present + 1);
}

TEST_CASE(taa_debug_view_is_validated_and_survives_pipeline_rebuild)
{
    ayt::render::Renderer renderer;
    CHECK(renderer.taaDebugView() == 0u);
    renderer.setTaaDebugView(3);
    renderer.setTaaEnabled(true);
    renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
    CHECK(renderer.taaDebugView() == 3u);
    CHECK(renderer.taaEnabled());
    renderer.setTaaDebugView(5);
    CHECK(renderer.taaDebugView() == 5u);
    renderer.setTaaDebugView(255);
    CHECK(renderer.taaDebugView() == 0u);
    CHECK(renderer.taaEnabled());
}

TEST_CASE(taa_fxaa_and_smaa_are_mutually_exclusive_and_survive_rebuild)
{
    ayt::render::Renderer renderer;
    CHECK(renderer.fxaaEnabled());
    CHECK_FALSE(renderer.smaaEnabled());
    CHECK_FALSE(renderer.taaEnabled());

    renderer.setTaaEnabled(true);
    CHECK(renderer.taaEnabled());
    CHECK_FALSE(renderer.fxaaEnabled());
    CHECK_FALSE(renderer.smaaEnabled());
    renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
    CHECK(renderer.taaEnabled());
    CHECK_FALSE(renderer.fxaaEnabled());
    CHECK_FALSE(renderer.smaaEnabled());

    renderer.setSmaaEnabled(true);
    CHECK(renderer.smaaEnabled());
    CHECK_FALSE(renderer.taaEnabled());
    CHECK_FALSE(renderer.fxaaEnabled());
    renderer.setFxaaEnabled(true);
    CHECK(renderer.fxaaEnabled());
    CHECK_FALSE(renderer.smaaEnabled());
    CHECK_FALSE(renderer.taaEnabled());
}

TEST_CASE(taa_halton_jitter_is_deterministic_centered_and_bounded)
{
    float sumX = 0.0f;
    float sumY = 0.0f;
    float maxAbsX = 0.0f;
    float maxAbsY = 0.0f;
    for (uint32_t i = 0; i < ayt::render::detail::TAAPass::kJitterSampleCount;
         ++i) {
        const auto first = ayt::render::detail::taaHaltonJitter(i);
        const auto second = ayt::render::detail::taaHaltonJitter(i);
        CHECK(std::abs(first.x - second.x) < 1.0e-7f);
        CHECK(std::abs(first.y - second.y) < 1.0e-7f);
        CHECK(first.x >= -0.5f);
        CHECK(first.x <= 0.5f);
        CHECK(first.y >= -0.5f);
        CHECK(first.y <= 0.5f);
        sumX += first.x;
        sumY += first.y;
        maxAbsX = std::max(maxAbsX, std::abs(first.x));
        maxAbsY = std::max(maxAbsY, std::abs(first.y));
    }
    CHECK(std::abs(sumX) < 0.5f);
    CHECK(std::abs(sumY) < 0.5f);
    CHECK(maxAbsX <= 0.5f * ayt::render::detail::TAAPass::kJitterSpread);
    CHECK(maxAbsY <= 0.5f * ayt::render::detail::TAAPass::kJitterSpread);
    const auto repeated = ayt::render::detail::taaHaltonJitter(
        ayt::render::detail::TAAPass::kJitterSampleCount);
    const auto first = ayt::render::detail::taaHaltonJitter(0);
    CHECK(std::abs(repeated.x - first.x) < 1.0e-7f);
    CHECK(std::abs(repeated.y - first.y) < 1.0e-7f);
}

TEST_CASE(taa_jitter_ramp_enters_temporal_sampling_without_a_hard_step)
{
    CHECK(std::abs(ayt::render::detail::taaJitterRampScale(0u) - 0.0f)
          < 1.0e-7f);
    CHECK(std::abs(ayt::render::detail::taaJitterRampScale(1u) - 0.25f)
          < 1.0e-7f);
    CHECK(std::abs(ayt::render::detail::taaJitterRampScale(2u) - 0.50f)
          < 1.0e-7f);
    CHECK(std::abs(ayt::render::detail::taaJitterRampScale(4u) - 1.0f)
          < 1.0e-7f);
    CHECK(std::abs(ayt::render::detail::taaJitterRampScale(40u) - 1.0f)
          < 1.0e-7f);
}

TEST_CASE(taa_perspective_jitter_is_depth_invariant_in_ndc)
{
    ayt::math::Float4x4 projection = ayt::math::Float4x4::zero();
    projection(0, 0) = 1.0f;
    projection(1, 1) = 1.0f;
    projection(2, 2) = 1.0f;
    projection(3, 2) = 1.0f;
    const auto jittered = ayt::render::detail::taaApplyProjectionJitter(
        projection, {0.25f, -0.25f}, 1000, 500);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if ((row == 0 || row == 1) && col == 2) {
                continue;
            }
            CHECK(std::abs(jittered(row, col) - projection(row, col))
                  < 1.0e-7f);
        }
    }
    CHECK(std::abs(jittered(0, 2) - 0.0005f) < 1.0e-7f);
    CHECK(std::abs(jittered(1, 2) - 0.0010f) < 1.0e-7f);
    CHECK(std::abs(projectedNdcComponent(jittered, 0, 0.0f, 0.0f, 2.0f)
                   - 0.0005f) < 1.0e-7f);
    CHECK(std::abs(projectedNdcComponent(jittered, 0, 0.0f, 0.0f, 20.0f)
                   - 0.0005f) < 1.0e-7f);
    CHECK(std::abs(projectedNdcComponent(jittered, 1, 0.0f, 0.0f, 2.0f)
                   - 0.0010f) < 1.0e-7f);
    CHECK(std::abs(projectedNdcComponent(jittered, 1, 0.0f, 0.0f, 20.0f)
                   - 0.0010f) < 1.0e-7f);
    const auto zeroSize = ayt::render::detail::taaApplyProjectionJitter(
        projection, {0.25f, -0.25f}, 0, 0);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            CHECK(std::abs(zeroSize(row, col) - projection(row, col))
                  < 1.0e-7f);
        }
    }
}

TEST_CASE(taa_orthographic_jitter_uses_translation_and_is_depth_invariant)
{
    const ayt::math::Float4x4 projection =
        ayt::math::Float4x4::identity();
    const auto jittered = ayt::render::detail::taaApplyProjectionJitter(
        projection, {0.25f, -0.25f}, 1000, 500);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if ((row == 0 || row == 1) && col == 3) {
                continue;
            }
            CHECK(std::abs(jittered(row, col) - projection(row, col))
                  < 1.0e-7f);
        }
    }
    CHECK(std::abs(jittered(0, 3) - 0.0005f) < 1.0e-7f);
    CHECK(std::abs(jittered(1, 3) - 0.0010f) < 1.0e-7f);
    CHECK(std::abs(projectedNdcComponent(jittered, 0, 0.0f, 0.0f, 2.0f)
                   - 0.0005f) < 1.0e-7f);
    CHECK(std::abs(projectedNdcComponent(jittered, 0, 0.0f, 0.0f, 20.0f)
                   - 0.0005f) < 1.0e-7f);
}

TEST_CASE(taa_noop_backend_returns_zero_and_lifecycle_is_idempotent)
{
    ayt::render::detail::TAAPass pass;
    ayt::render::detail::BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    ayt::render::detail::FrameContext frame{};
    ayt::render::detail::PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 640, 360, frame, 0
    };
    CHECK_FALSE(pass.prepareFrame(adapter, pool, frame.view, frame.projection,
                                  frame.cameraPosition, 640, 360));
    CHECK(pass.execute(ctx) == 0u);
    pass.invalidateHistory();
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
}

TEST_CASE(taa_shader_contract_has_reprojection_neighborhood_clamp_and_motion_feedback)
{
    const std::string source = ayt::render::detail::taaPhoskiaSourceForTests();
    CHECK(source.find("material TemporalAA") != std::string::npos);
    CHECK(source.find("out vUv : texcoord = uv") != std::string::npos);
    CHECK(source.find("texture2d currentColor") != std::string::npos);
    CHECK(source.find("texture2d historyColor") != std::string::npos);
    CHECK(source.find("texture2d worldPosition") != std::string::npos);
    CHECK(source.find("texture2d geometryData") != std::string::npos);
    CHECK(source.find("texture2d motionVectors") != std::string::npos);
    CHECK(source.find("uniform mat4 previousViewProjection")
          == std::string::npos);
    CHECK(source.find("uniform mat4 currentViewProjection")
          != std::string::npos);
    CHECK(source.find("uniform vec4 taaJitter") != std::string::npos);
    CHECK(source.find("uniform vec4 taaDepthParams") != std::string::npos);
    CHECK(source.find("neighborhoodMin") != std::string::npos);
    CHECK(source.find("expectedPreviousDepth = motion.z") != std::string::npos);
    CHECK(source.find("coverage > 0.5") != std::string::npos);
    CHECK(source.find("previousUv = outputUv - motion.xy")
          != std::string::npos);
    CHECK(source.find("validHistory && motion.w > 0.5") != std::string::npos);
    CHECK(source.find("taaJitter.w > 0.5") != std::string::npos);
    CHECK(source.find("outputUv.x + taaJitter.x") != std::string::npos);
    CHECK(source.find("outputUv.y + taaJitter.y") != std::string::npos);
    CHECK(source.find("taaJitter.z > 0.5") != std::string::npos);
    CHECK(source.find("motionAmount") != std::string::npos);
    CHECK(source.find("sample(sceneDepth, uv)") != std::string::npos);
    CHECK(source.find("sample(geometryData, surfaceUv)") != std::string::npos);
    CHECK(source.find("sample(worldPosition, surfaceUv)") != std::string::npos);
    CHECK(source.find("sample(motionVectors, surfaceUv)") != std::string::npos);
    CHECK(source.find("abs(unclippedHistory.y - history.y)") != std::string::npos);
    CHECK(source.find("unexpectedMismatch") != std::string::npos);
    CHECK(source.find("luminanceMismatch") != std::string::npos);
    CHECK(source.find("currentHistoryDepth") != std::string::npos);
    CHECK(source.find("expectedPreviousDepth") != std::string::npos);
    CHECK(source.find("vec2(floor(historyPixel.x), floor(historyPixel.y))") != std::string::npos);
    CHECK(source.find("h0.w < 0.0") != std::string::npos);
    CHECK(source.find("h3.w >= 0.0") != std::string::npos);
    CHECK(source.find("reactiveMismatch") != std::string::npos);
    CHECK(std::string(ayt::render::detail::kTaaCacheKeyCStr)
          == "taa_phoskia_supported_history_v10");
    CHECK(source.find("currentHistoryDepth = selectedDepth") != std::string::npos);
    CHECK(source.find("historyDepthTolerance = taaDepthParams.x + max(motion.w - 1.0, 0.0) * 2.0") != std::string::npos);
    CHECK(source.find("historyConfidence = clamp(historyWeight, 0.0, 1.0)") != std::string::npos);
    CHECK(source.find("effectiveFeedback = clamp(feedback, 0.0, 0.95) * historyConfidence") != std::string::npos);
}

TEST_CASE(taa_failed_resolve_discards_motion_snapshots)
{
    ayt::render::detail::TAAPass taa;
    ayt::render::detail::MotionVectorPass motion;
    // Simulate motion having committed an object before a downstream failure.
    auto& snapshots = const_cast<ayt::render::detail::MotionHistoryCache&>(
        motion.historyForTests());
    snapshots.commit(7, 11, ayt::math::Float4x4::identity(), nullptr, 0, 1);
    CHECK(snapshots.size() == 1u);
    taa.finishFrame(&motion);
    CHECK(snapshots.size() == 0u);
    CHECK_FALSE(taa.historyValid());
    CHECK_FALSE(taa.producedThisFrame());
    CHECK_FALSE(motion.hasPreviousFrame());
    taa.finishFrame(nullptr);
}

TEST_CASE(taa_failed_resolve_invalidates_blackboard_with_producer_reason)
{
    ayt::render::detail::TAAPass taa;
    ayt::render::detail::MotionVectorPass motion;
    ayt::render::detail::RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    blackboard.publish(
        ayt::render::detail::BlackboardResourceId::TaaHistoryRead,
        ayt::render::detail::BlackboardResourceLifetime::PersistentHistory,
        bgfx::FrameBufferHandle{7}, BGFX_INVALID_HANDLE,
        640, 360, 1, true);

    taa.finishFrame(&motion, &blackboard);
    const auto* history = blackboard.find(
        ayt::render::detail::BlackboardResourceId::TaaHistoryRead);
    CHECK(history != nullptr);
    CHECK_FALSE(history->contentValid);
    CHECK(history->invalidationReason
          == ayt::render::detail::ResourceInvalidationReason::ProducerFailure);
}

TEST_CASE(taa_phoskia_source_compiles_for_d3d11_and_d3d12)
{
    if (!taaFileExists(AY_SHADER_SHADERC_HINT)) {
        std::cerr << "[TAAPass test] SKIP: shaderc unavailable.\n";
        return;
    }
    ayt::shader::AYShadercDriver::setDefaultExecutable(
        AY_SHADER_SHADERC_HINT);
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions options;
    options.keepSources = true;
    ayt::shader::BGFXCompileOptions bgfxOptions;
    bgfxOptions.shadercPath = AY_SHADER_SHADERC_HINT;
    bgfxOptions.platform = "windows";
    bgfxOptions.profile = "s_5_0";
#ifdef AY_SHADER_BGFX_COMMON_HINT
    bgfxOptions.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    bgfxOptions.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    ayt::shader::CompiledShaderProgram program;
    compiler.compileToProgram(
        ayt::render::detail::taaPhoskiaSourceForTests(),
        options, bgfxOptions, program);
    bool correctInverseGrouping = false;
    for (const auto& source : program.sources) {
        correctInverseGrouping = correctInverseGrouping || source.second.find(
            "((resolved.x - resolved.y) - resolved.z)") != std::string::npos;
    }
    CHECK(correctInverseGrouping);
    if (!program.success) {
        std::cerr << "[TAAPass test] Phoskia compile failed:\n";
        for (const std::string& error : program.errors) {
            std::cerr << "  " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());
}

TEST_SUITE_END
