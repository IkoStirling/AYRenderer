#include "detail/RenderPipelineLifecycle.h"
#include "detail/RenderPipeline.h"
#include "detail/PresentationPass.h"
#include "detail/AutoExposurePass.h"
#include "detail/BloomBlurPass.h"
#include "detail/BloomExtractPass.h"
#include "detail/DeferredDecalPass.h"
#include "detail/DepthHazePass.h"
#include "detail/EditorOverlayPass.h"
#include "detail/FXAAPass.h"
#include "detail/GBufferDebugPass.h"
#include "detail/GBufferPass.h"
#include "detail/LightingPass.h"
#include "detail/MotionVectorPass.h"
#include "detail/PostProcessPass.h"
#include "detail/PresentPass.h"
#include "detail/ShadowPass.h"
#include "detail/SkyboxPass.h"
#include "detail/SMAAPass.h"
#include "detail/SSAOPass.h"
#include "detail/SsrPass.h"
#include "detail/TAAPass.h"
#include "detail/TransparentPass.h"
#include "detail/VolumetricPass.h"
#include "detail/CloudPass.h"

namespace ayt::render::detail {
namespace {
template<class Pass>
void destroy(RenderPipeline& pipeline, BGFXAdapter& adapter)
{
    if (auto* pass = pipeline.findPass<Pass>()) pass->destroyResources(adapter);
}
}

void destroyAttachmentBorrowerResources(RenderPipeline& pipeline, BGFXAdapter& adapter)
{
    if (!adapter.isInitialized()) return;
    destroy<TransparentPass>(pipeline, adapter);
    destroy<MotionVectorPass>(pipeline, adapter);
    destroy<DeferredDecalPass>(pipeline, adapter);
}

void destroyMainAttachmentResources(RenderPipeline& pipeline, BGFXAdapter& adapter)
{
    if (!adapter.isInitialized()) return;
    destroyAttachmentBorrowerResources(pipeline, adapter);
    destroy<ShadowPass>(pipeline, adapter);
    destroy<GBufferPass>(pipeline, adapter);
    destroy<LightingPass>(pipeline, adapter);
}

void destroyPipelineGpuResources(RenderPipeline& pipeline, BGFXAdapter& adapter)
{
    if (!adapter.isInitialized()) return;
    destroyMainAttachmentResources(pipeline, adapter);
    destroy<SkyboxPass>(pipeline, adapter);
    destroy<BloomExtractPass>(pipeline, adapter);
    destroy<BloomBlurPass>(pipeline, adapter);
    destroy<AutoExposurePass>(pipeline, adapter);
    destroy<SsrPass>(pipeline, adapter);
    destroy<VolumetricPass>(pipeline, adapter);
    destroy<CloudPass>(pipeline, adapter);
    destroy<DepthHazePass>(pipeline, adapter);
    destroy<SSAOPass>(pipeline, adapter);
    destroy<GBufferDebugPass>(pipeline, adapter);
    destroy<PostProcessPass>(pipeline, adapter);
    destroy<FXAAPass>(pipeline, adapter);
    destroy<SMAAPass>(pipeline, adapter);
    destroy<TAAPass>(pipeline, adapter);
    destroyPresentationResources(pipeline, adapter);
    destroy<PresentPass>(pipeline, adapter);
    destroy<EditorOverlayPass>(pipeline, adapter);
    // ForwardOpaque/Forward2DOpaque own no private GPU objects. UIPass borrows
    // its backend; the Host shuts that backend down before its Renderer dies.
}
}
