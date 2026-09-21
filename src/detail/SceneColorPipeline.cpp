#include "detail/SceneColorPipeline.h"

#include "detail/BGFXAdapter.h"
#include "detail/DepthHazePass.h"
#include "detail/FgResource.h"
#include "detail/GBufferPass.h"
#include "detail/LightingPass.h"
#include "detail/PassExecContext.h"
#include "detail/RenderResourceBlackboard.h"

namespace ayt::render::detail
{

bgfx::FrameBufferHandle selectSceneColorSourceFbo(
    const PassExecContext& ctx) noexcept
{
    const bool deferredPath = ctx.gbufferPass != nullptr
        && ctx.lightingPass != nullptr;
    BlackboardGBufferView blackboardGBuffer;
    const bool gbufferProduced = ctx.resourceBlackboard != nullptr
        ? ctx.resourceBlackboard->resolveGBuffer(blackboardGBuffer)
        : ctx.gbufferPass != nullptr
            && ctx.gbufferPass->producedThisFrame();
    const BlackboardResourceEntry* lighting =
        ctx.resourceBlackboard != nullptr
            ? ctx.resourceBlackboard->findProduced(
                  BlackboardResourceId::LightingColor)
            : nullptr;
    const bool lightingProduced = ctx.resourceBlackboard != nullptr
        ? lighting != nullptr
        : ctx.lightingPass != nullptr
            && ctx.lightingPass->producedThisFrame();
    if (deferredPath
        && (!gbufferProduced || !lightingProduced)) {
        // A mounted deferred path must fail closed. Falling back to sceneFbo
        // would expose an unwritten/stale forward target and hide a producer
        // failure.
        return BGFX_INVALID_HANDLE;
    }

    // Haze becomes authoritative only after a successful current-frame
    // submit. Persistent FrameGraph handles alone do not prove freshness.
    if (ctx.resourceBlackboard != nullptr) {
        const BlackboardResourceEntry* haze =
            ctx.resourceBlackboard->findProduced(
                BlackboardResourceId::DepthHazeColor);
        if (haze != nullptr && BGFXAdapter::isValid(haze->framebuffer)) {
            return haze->framebuffer;
        }
    } else if (ctx.depthHazePass != nullptr
               && ctx.depthHazePass->producedThisFrame()
               && ctx.frameGraph != nullptr) {
        const bgfx::FrameBufferHandle hazeSource =
            ctx.frameGraph->resolveSemantic(FgSemantic::HazeSource);
        if (BGFXAdapter::isValid(hazeSource)) {
            return hazeSource;
        }
    }

    if (deferredPath && lighting != nullptr
        && BGFXAdapter::isValid(lighting->framebuffer)) {
        return lighting->framebuffer;
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
