#include "AYTest.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/LightingPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"

#include "AYRenderer/RenderScene.h"
#include "AYShader/ShaderResourcePool.h"

#include <unordered_map>

namespace {

struct ContractStubs {
    ayt::render::detail::BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    ayt::render::RenderScene scene;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMesh> meshes;
    std::unordered_map<uint64_t, ayt::render::detail::GpuTexture> textures;
    std::unordered_map<uint64_t, ayt::render::detail::GpuMaterial> materials;
    ayt::render::detail::FrameContext frame;
};

} // namespace

TEST_SUITE(AYRenderer_GBufferContractHardening)

TEST_CASE(producer_frame_state_defaults_false_and_size_broadcast_resets_it)
{
    ayt::render::detail::GBufferPass gbuffer;
    ayt::render::detail::LightingPass lighting;

    CHECK_FALSE(gbuffer.producedThisFrame());
    CHECK_FALSE(lighting.producedThisFrame());
    gbuffer.setGbufferSize(1280, 720);
    lighting.setOutputSize(1280, 720);
    CHECK_FALSE(gbuffer.producedThisFrame());
    CHECK_FALSE(lighting.producedThisFrame());
}

TEST_CASE(lighting_prepare_is_safe_before_backend_initialization)
{
    ayt::render::detail::BGFXAdapter adapter;
    ayt::render::detail::LightingPass lighting;
    lighting.setOutputSize(1280, 720);
    lighting.prepareOutput(adapter);
    CHECK_FALSE(bgfx::isValid(lighting.lightingOutputFbo()));
    CHECK_FALSE(lighting.producedThisFrame());
}

TEST_CASE(deferred_source_never_falls_back_to_forward_scene_when_not_produced)
{
    using namespace ayt::render::detail;
    ContractStubs stubs;
    GBufferPass gbuffer;
    LightingPass lighting;
    PassExecContext ctx{
        stubs.adapter, stubs.pool, stubs.scene,
        stubs.meshes, stubs.textures, stubs.materials,
        0, 0, 1280, 720, stubs.frame, 0u
    };
    ctx.gbufferPass = &gbuffer;
    ctx.lightingPass = &lighting;
    ctx.sceneFbo.idx = 7u;

    CHECK_FALSE(bgfx::isValid(PostProcessPass::selectSourceFbo(ctx)));
}

TEST_CASE(forward_source_keeps_scene_fbo_fallback)
{
    using namespace ayt::render::detail;
    ContractStubs stubs;
    PassExecContext ctx{
        stubs.adapter, stubs.pool, stubs.scene,
        stubs.meshes, stubs.textures, stubs.materials,
        0, 0, 1280, 720, stubs.frame, 0u
    };
    ctx.sceneFbo.idx = 9u;

    CHECK(PostProcessPass::selectSourceFbo(ctx).idx == 9u);
}

TEST_SUITE_END
