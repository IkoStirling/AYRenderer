#include "detail/RenderPipelineFactory.h"
#include "detail/PresentationStageCatalog.h"
#include "detail/AutoExposurePass.h"
#include "detail/BloomBlurPass.h"
#include "detail/BloomExtractPass.h"
#include "detail/DeferredDecalPass.h"
#include "detail/DepthHazePass.h"
#include "detail/EditorOverlayPass.h"
#include "detail/Forward2DOpaquePass.h"
#include "detail/ForwardOpaquePass.h"
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
#include "detail/UIPass.h"
#include "detail/VolumetricPass.h"
#include "detail/CloudPass.h"

namespace ayt::render::detail {

std::unique_ptr<RenderPass> makeRenderPass(RenderPassSlot slot)
{
    switch (slot) {
#define AY_PRESENTATION_STAGE(Slot, Output, View, Order, Inputs, Point, History, Job, Direct, PreViews) \
    case RenderPassSlot::Slot: return makePresentationStage<RenderPassSlot::Slot>();
#include "detail/PresentationStages.def"
#undef AY_PRESENTATION_STAGE
    case RenderPassSlot::Shadow: return std::make_unique<ShadowPass>();
    case RenderPassSlot::ForwardOpaque: return std::make_unique<ForwardOpaquePass>();
    case RenderPassSlot::Transparent: return std::make_unique<TransparentPass>();
    case RenderPassSlot::PostProcess: return std::make_unique<PostProcessPass>();
    case RenderPassSlot::FXAA: return std::make_unique<FXAAPass>();
    case RenderPassSlot::SMAA: return std::make_unique<SMAAPass>();
    case RenderPassSlot::TAA: return std::make_unique<TAAPass>();
    case RenderPassSlot::MotionVector: return std::make_unique<MotionVectorPass>();
    case RenderPassSlot::AutoExposure: return std::make_unique<AutoExposurePass>();
    case RenderPassSlot::Present: return std::make_unique<PresentPass>();
    case RenderPassSlot::EditorOverlay: return std::make_unique<EditorOverlayPass>();
    case RenderPassSlot::UI: return std::make_unique<UIPass>();
    case RenderPassSlot::GBuffer: return std::make_unique<GBufferPass>();
    case RenderPassSlot::Lighting: return std::make_unique<LightingPass>();
    case RenderPassSlot::Skybox: return std::make_unique<SkyboxPass>();
    case RenderPassSlot::BloomExtract: return std::make_unique<BloomExtractPass>();
    case RenderPassSlot::BloomBlur: return std::make_unique<BloomBlurPass>();
    case RenderPassSlot::DepthHaze: return std::make_unique<DepthHazePass>();
    case RenderPassSlot::SSR: return std::make_unique<SsrPass>();
    case RenderPassSlot::Volumetric: return std::make_unique<VolumetricPass>();
    case RenderPassSlot::Clouds: return std::make_unique<CloudPass>();
    case RenderPassSlot::DeferredDecal: return std::make_unique<DeferredDecalPass>();
    case RenderPassSlot::SSAO: return std::make_unique<SSAOPass>();
    case RenderPassSlot::GBufferDebug: return std::make_unique<GBufferDebugPass>();
    case RenderPassSlot::Forward2DOpaque: return std::make_unique<Forward2DOpaquePass>();
    }
    return nullptr;
}

} // namespace ayt::render::detail
