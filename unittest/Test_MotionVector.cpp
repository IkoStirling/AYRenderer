#include "AYTest.h"

#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShadercDriver.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/LightingPass.h"
#include "detail/MotionVectorPass.h"
#include "detail/PassExecContext.h"
#include "detail/RenderViewOrder.h"
#include "detail/SSAOPass.h"

#include <algorithm>
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
using ayt::render::detail::MotionHistoryCache;
using ayt::render::detail::MotionVectorPass;

std::size_t passIndex(const RenderPipelineDesc& desc, RenderPassSlot slot)
{
    const auto it = std::find(desc.passes.begin(), desc.passes.end(), slot);
    return it == desc.passes.end()
        ? desc.passes.size()
        : static_cast<std::size_t>(it - desc.passes.begin());
}

bool motionFileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

} // namespace

TEST_SUITE(AYRenderer_MotionVector)

TEST_CASE(motion_vector_append_only_abi_format_and_view_are_locked)
{
    CHECK(static_cast<uint8_t>(RenderPassSlot::MotionVector) == 20u);
    CHECK(MotionVectorPass::kMotionVectorViewId == 3u);
    CHECK(MotionVectorPass::kVelocityFormat == bgfx::TextureFormat::RG16F);
    CHECK(std::is_final_v<MotionVectorPass>);

    ayt::render::DrawItem item;
    CHECK(item.motionObjectId == 0u);
    item.motionObjectId = 0x123456789abcdef0ull;
    CHECK(item.motionObjectId == 0x123456789abcdef0ull);
}

TEST_CASE(motion_vector_is_deferred_only_between_gbuffer_and_ssao)
{
    CHECK_FALSE(RenderPipelineDesc::makeDefault().contains(
        RenderPassSlot::MotionVector));
    CHECK_FALSE(RenderPipelineDesc::makeEditorForward().contains(
        RenderPassSlot::MotionVector));
    for (const RenderPipelineDesc desc : {
             RenderPipelineDesc::makeDeferred(),
             RenderPipelineDesc::makeEditorDeferred()}) {
        const std::size_t gbuffer = passIndex(desc, RenderPassSlot::GBuffer);
        const std::size_t motion = passIndex(desc, RenderPassSlot::MotionVector);
        const std::size_t ssao = passIndex(desc, RenderPassSlot::SSAO);
        const std::size_t lighting = passIndex(desc, RenderPassSlot::Lighting);
        CHECK(motion == gbuffer + 1u);
        CHECK(ssao == motion + 1u);
        CHECK(lighting == ssao + 1u);
    }
}

TEST_CASE(motion_vector_view_executes_after_gbuffer_before_ssao_and_lighting)
{
    const auto& order = ayt::render::detail::kRenderViewOrder;
    const auto gbuffer = std::find(order.begin(), order.end(),
        ayt::render::detail::GBufferPass::kGBufferViewId);
    const auto motion = std::find(order.begin(), order.end(),
        MotionVectorPass::kMotionVectorViewId);
    const auto ssao = std::find(order.begin(), order.end(),
        ayt::render::detail::SSAOPass::kSsaoViewId);
    const auto lighting = std::find(order.begin(), order.end(),
        ayt::render::detail::LightingPass::kLightingViewId);
    CHECK(gbuffer != order.end());
    CHECK(motion == gbuffer + 1);
    CHECK(ssao == motion + 1);
    CHECK(lighting == ssao + 1);
    CHECK(std::count(order.begin(), order.end(),
                     MotionVectorPass::kMotionVectorViewId) == 1);
}

TEST_CASE(motion_history_requires_consecutive_frame_matching_mesh_and_pose_shape)
{
    MotionHistoryCache cache;
    ayt::math::Float4x4 world = ayt::math::Float4x4::identity();
    world(0, 3) = 3.0f;
    ayt::math::Float4x4 bones[2] = {
        ayt::math::Float4x4::identity(),
        ayt::math::Float4x4::identity(),
    };
    bones[1](1, 3) = 2.0f;

    cache.commit(10u, 20u, world, bones, 2u, 1u);
    CHECK(cache.size() == 1u);
    const auto* previous = cache.find(10u, 20u, 2u, 2u);
    CHECK(previous != nullptr);
    CHECK(previous->world(0, 3) == 3.0f);
    CHECK(previous->bones.size() == 2u);
    CHECK(previous->bones[1](1, 3) == 2.0f);
    CHECK(cache.find(10u, 21u, 2u, 2u) == nullptr);
    CHECK(cache.find(11u, 20u, 2u, 2u) == nullptr);
    CHECK(cache.find(10u, 20u, 1u, 2u) == nullptr);
    CHECK(cache.find(10u, 20u, 2u, 3u) == nullptr);
    CHECK(cache.find(0u, 20u, 2u, 2u) == nullptr);

    cache.pruneBefore(2u);
    CHECK(cache.size() == 0u);
    cache.commit(10u, 20u, world, nullptr, 0u, 4u);
    CHECK(cache.find(10u, 20u, 0u, 5u) != nullptr);
    cache.clear();
    CHECK(cache.size() == 0u);
}

TEST_CASE(motion_vector_shader_contract_covers_rigid_skin_cutout_and_sentinel)
{
    const std::string opaque =
        ayt::render::detail::motionVectorPhoskiaSourceForTests();
    const std::string cutout =
        ayt::render::detail::motionVectorCutoutPhoskiaSourceForTests();

    CHECK(opaque.find("uniform mat4 bones[128]") != std::string::npos);
    CHECK(opaque.find("uniform mat4 previousBones[128]")
          != std::string::npos);
    CHECK(opaque.find("uniform mat4 previousWorld") != std::string::npos);
    CHECK(opaque.find("uniform mat4 previousViewProjection")
          != std::string::npos);
    CHECK(opaque.find("skinningMatrix(boneId, boneWt, bones")
          != std::string::npos);
    CHECK(opaque.find("skinningMatrix(boneId, boneWt, previousBones")
          != std::string::npos);
    CHECK(opaque.find("let velocity = vec2(2.0, 2.0)")
          != std::string::npos);
    CHECK(opaque.find("velocity = currentUv - previousUv")
          != std::string::npos);
    CHECK(cutout.find("texture2d opacityMap") != std::string::npos);
    CHECK(cutout.find("discard") != std::string::npos);
    CHECK(std::string(ayt::render::detail::kMotionVectorCacheKeyCStr)
          == "motion_vector_phoskia_rg16f_rigid_skin_v3");
}

TEST_CASE(motion_vector_noop_backend_returns_zero_and_lifecycle_is_idempotent)
{
    MotionVectorPass pass;
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
    pass.setOutputSize(640, 360);
    pass.setRequestedThisFrame(true);
    CHECK(pass.requestedThisFrame());
    CHECK(pass.execute(ctx) == 0u);
    CHECK_FALSE(pass.producedThisFrame());
    pass.invalidateHistory();
    pass.destroyResources(adapter);
    pass.destroyResources(adapter);
}

TEST_CASE(motion_vector_phoskia_sources_compile_for_d3d11_and_d3d12)
{
    if (!motionFileExists(AY_SHADER_SHADERC_HINT)) {
        std::cerr << "[MotionVectorPass test] SKIP: shaderc unavailable.\n";
        return;
    }
    ayt::shader::AYShadercDriver::setDefaultExecutable(
        AY_SHADER_SHADERC_HINT);
    auto compileSource = [](const char* source, const char* label) {
        ayt::shader::phoskia::Compiler compiler;
        ayt::shader::phoskia::CompileOptions options;
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
        compiler.compileToProgram(source, options, bgfxOptions, program);
        if (!program.success) {
            std::cerr << "[MotionVectorPass test] Phoskia " << label
                      << " failed:\n";
            for (const std::string& error : program.errors) {
                std::cerr << "  " << error << '\n';
            }
        }
        const auto hasBoneArray = [&program](const char* name) {
            return std::any_of(
                program.uniforms.begin(), program.uniforms.end(),
                [name](const ayt::shader::BGFXUniform& uniform) {
                    return uniform.name == name && uniform.type == "mat4"
                        && uniform.count == 128u;
                });
        };
        const bool boneBindings = hasBoneArray("bones")
            && hasBoneArray("previousBones");
        if (program.success && !boneBindings) {
            std::cerr << "[MotionVectorPass test] " << label
                      << " omitted current/previous bone array bindings\n";
        }
        return program.success && !program.vsBin.empty()
            && !program.fsBin.empty() && boneBindings;
    };
    CHECK(compileSource(
        ayt::render::detail::motionVectorPhoskiaSourceForTests(), "opaque"));
    CHECK(compileSource(
        ayt::render::detail::motionVectorCutoutPhoskiaSourceForTests(),
        "cutout"));
}

TEST_SUITE_END
