#include "detail/SceneColorPipeline.h"

#include "detail/BGFXAdapter.h"
#include "detail/DepthHazePass.h"
#include "detail/FgResource.h"
#include "detail/GBufferPass.h"
#include "detail/LightingPass.h"
#include "detail/PassExecContext.h"

namespace ayt::render::detail
{

bgfx::FrameBufferHandle selectSceneColorSourceFbo(
    const PassExecContext& ctx) noexcept
{
    const bool deferredPath = ctx.gbufferPass != nullptr
        && ctx.lightingPass != nullptr;
    if (deferredPath
        && (!ctx.gbufferPass->producedThisFrame()
            || !ctx.lightingPass->producedThisFrame())) {
        // A mounted deferred path must fail closed. Falling back to sceneFbo
        // would expose an unwritten/stale forward target and hide a producer
        // failure.
        return BGFX_INVALID_HANDLE;
    }

    // Haze becomes authoritative only after a successful current-frame
    // submit. Persistent FrameGraph handles alone do not prove freshness.
    if (ctx.depthHazePass != nullptr
        && ctx.depthHazePass->producedThisFrame()
        && ctx.frameGraph != nullptr) {
        const bgfx::FrameBufferHandle hazeSource =
            ctx.frameGraph->resolveSemantic(FgSemantic::HazeSource);
        if (BGFXAdapter::isValid(hazeSource)) {
            return hazeSource;
        }
    }

    if (ctx.frameGraph != nullptr) {
        const bgfx::FrameBufferHandle graphSource =
            ctx.frameGraph->resolveSemantic(FgSemantic::FinalColorSource);
        if (BGFXAdapter::isValid(graphSource)) {
            return graphSource;
        }
    }

    // Legacy hand-built contexts may omit FrameGraph. Preserve the deferred
    // producer priority before accepting the shared forward scene target.
    if (deferredPath) {
        const bgfx::FrameBufferHandle lightingFbo =
            ctx.lightingPass->lightingOutputFbo();
        if (BGFXAdapter::isValid(lightingFbo)) {
            return lightingFbo;
        }
    }

    return BGFXAdapter::isValid(ctx.sceneFbo)
        ? ctx.sceneFbo
        : bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
}

} // namespace ayt::render::detail
