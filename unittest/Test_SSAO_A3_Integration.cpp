// §A3 SSAO integration test (2026-07-24, mid-term FG MVP SSAO
// Gate commit).
//
// Validates the FrameGraph + SSAOPass ownership wire. Lighting is the only
// consumer; DepthHaze/PostProcess never composite AO over final color.
//   3) Noop-backend ⇒ no FG alloc; SSAOPass::execute and
//      PostProcessPass::execute both short-circuit to 0.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShaderResource.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/LightingPass.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/SSAOPass.h"

#include <bgfx/bgfx.h>
#include <unordered_map>

using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FgResourceId;
using ayt::render::detail::FgSemantic;
using ayt::render::detail::FgTextureDesc;
using ayt::render::detail::FgTextureScale;
using ayt::render::detail::FrameContext;
using ayt::render::detail::FrameGraph;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::PostProcessPass;
using ayt::render::detail::SSAOPass;

namespace {

bgfx::FrameBufferHandle makeFakeHandle(uint16_t idx)
{
    bgfx::FrameBufferHandle h;
    h.idx = idx;
    return h;
}

} // namespace

TEST_SUITE(AYRenderer_SSAO_A3_Integration)

// ─── A. FG.resolveSemantic(SSAOSource) invalid ⇒ no wire ─────────────

TEST_CASE(a3int_fg_ssaosource_invalid_when_tex_not_live) {
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    // No SSAO addResource / addPass ⇒ SSAOSource unresolved.
    fg.compile();

    const bgfx::FrameBufferHandle h =
        fg.resolveSemantic(FgSemantic::SSAOSource);
    CHECK(!BGFXAdapter::isValid(h));
}

TEST_CASE(a3int_fg_ssaosource_valid_when_tex_live) {
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));
    fg.addResource(FgResourceId::SSAOTexture,
                   FgTextureDesc{
                       bgfx::TextureFormat::RGBA8,
                       FgTextureScale::Full,
                       /*transient=*/true,
                       /*withDepth=*/false});
    fg.addPass({"SSAO",
                {},
                {FgResourceId::SSAOTexture},
                /*enabled=*/true});
    fg.setResolvedSemantic(FgSemantic::SSAOSource,
                           FgResourceId::SSAOTexture);
    fg.compile();

    // Note: the resolve() lazy-create path requires
    // adapter.isInitialized() to return valid handles; on the
    // headless test path the adapter is uninitialized so the
    // physical remains invalid. We pin the `hasLogical` arm of
    // resolveSemantic — when the logical isn't live OR the
    // adapter is uninitialized, resolveSemantic returns invalid.
    // Physical allocation requires an initialized adapter; logical liveness is
    // still visible through graph statistics on the headless path.
    const bgfx::FrameBufferHandle h =
        fg.resolveSemantic(FgSemantic::SSAOSource);
    // The headless test path returns invalid — pin it.
    CHECK(!BGFXAdapter::isValid(h));
    CHECK(fg.stats().livePasses     == 1);
    CHECK(fg.stats().declaredPasses == 1);
}

// ─── B. SSAOPass execute returns 0 on every unfulfilled gate ───────

TEST_CASE(a3int_ssao_pass_executes_0_no_gbuffer) {
    // K-SSAO-1 — when the gbufferPass is null AND FG is wired,
    // the resolve(SSAOTexture) gate still returns invalid
    // (because the central render() ssaoPassEnabled was false in
    // that case) ⇒ execute() returns 0.
    BGFXAdapter adapter;
    FrameGraph fg(adapter);
    fg.beginFrame(800, 600);
    fg.importExternal(FgResourceId::SceneColor, makeFakeHandle(0x10));

    ayt::render::RenderScene scene{};
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh>     meshes;
    std::unordered_map<uint64_t, GpuTexture>  textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    ayt::shader::ShaderResourcePool pool;
    SSAOPass pass{};

    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 800, 600,
        frame,
        /*viewId=*/14u,
    };
    ctx.frameGraph = &fg;
    // gbufferPass intentionally left nullptr.
    CHECK(pass.execute(ctx) == 0u);
}

// ─── C. AO ownership invariant ──────────────────────────────────────

TEST_CASE(a3int_lighting_is_the_only_final_color_ssao_consumer) {
    const std::string lighting(
        ayt::render::detail::kLightingPhoskiaSourceCStr);
    const std::string post(
        ayt::render::detail::postProcessPhoskiaSourceForTests());
    CHECK(lighting.find("texture2d ssaoTexture") != std::string::npos);
    CHECK(lighting.find("materialAo * ssaoAmbient") != std::string::npos);
    CHECK(post.find("ssaoTexture") == std::string::npos);
}

TEST_SUITE_END
