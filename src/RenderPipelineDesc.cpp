#include "AYRenderer/RenderTypes.h"
#include "detail/RenderPipelinePolicy.h"
#include <algorithm>

namespace ayt::render {

RenderPipelineDesc RenderPipelineDesc::makeDefault()
{
    // Product composition only. Mounted stages still obey feature settings,
    // prerequisites and current-frame graph liveness during execution.
    return RenderPipelineDesc{{
        RenderPassSlot::Shadow,
        RenderPassSlot::ForwardOpaque,
        // Forward has no GBuffer, so DepthHaze remains a zero-cost no-op. Its
        // position matches the deferred dependency order for custom pipelines.
        RenderPassSlot::DepthHaze,
        RenderPassSlot::Transparent,
        // Camera-overlay 2D composes after all 3D surfaces and before the HDR
        // post chain. Zero payload items remain a zero-draw fast path.
        RenderPassSlot::Forward2DOpaque,
        RenderPassSlot::BloomExtract,
        RenderPassSlot::BloomBlur,
        RenderPassSlot::AutoExposure,
        RenderPassSlot::PostProcess,
        RenderPassSlot::FXAA,
        RenderPassSlot::SMAA,
        RenderPassSlot::StylizedOutline,
        RenderPassSlot::ColorGrading,
        RenderPassSlot::ColorStylization,
        RenderPassSlot::TemporalFeedback,
        RenderPassSlot::AnalogDisplay,
        RenderPassSlot::Present,
        RenderPassSlot::UI,
    }};
}

RenderPipelineDesc RenderPipelineDesc::makeForwardWithShadows()
{
    // Keep the historical Host spelling as an exact alias.
    return makeDefault();
}

namespace {

void ensurePresentAfterPostProcess(RenderPipelineDesc& desc)
{
    auto ppIt = std::find(desc.passes.begin(), desc.passes.end(),
                          RenderPassSlot::PostProcess);
    const auto presentIt = std::find(desc.passes.begin(), desc.passes.end(),
                                     RenderPassSlot::Present);
    if (ppIt != desc.passes.end() && presentIt == desc.passes.end()) {
        auto insertAfter = ppIt;
        const auto fxaaIt = std::find(desc.passes.begin(), desc.passes.end(),
                                      RenderPassSlot::FXAA);
        if (fxaaIt != desc.passes.end() && fxaaIt > insertAfter) {
            insertAfter = fxaaIt;
        }
        const auto feedbackIt=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::TemporalFeedback);
        if(feedbackIt!=desc.passes.end() && feedbackIt>insertAfter)insertAfter=feedbackIt;
        const auto displayIt=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::AnalogDisplay);
        if(displayIt!=desc.passes.end() && displayIt>insertAfter)insertAfter=displayIt;
        const auto outlineIt=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::StylizedOutline);
        if(outlineIt!=desc.passes.end() && outlineIt>insertAfter)insertAfter=outlineIt;
        const auto gradingIt = std::find(desc.passes.begin(), desc.passes.end(),
                                         RenderPassSlot::ColorGrading);
        if (gradingIt != desc.passes.end() && gradingIt > insertAfter) {
            insertAfter = gradingIt;
        }
        const auto styleIt = std::find(desc.passes.begin(), desc.passes.end(),
                                        RenderPassSlot::ColorStylization);
        if (styleIt != desc.passes.end() && styleIt > insertAfter) {
            insertAfter = styleIt;
        }
        const auto smaaIt = std::find(desc.passes.begin(), desc.passes.end(),
                                      RenderPassSlot::SMAA);
        if (smaaIt != desc.passes.end() && smaaIt > insertAfter) {
            insertAfter = smaaIt;
        }
        const auto taaIt = std::find(desc.passes.begin(), desc.passes.end(),
                                     RenderPassSlot::TAA);
        if (taaIt != desc.passes.end() && taaIt > insertAfter) {
            insertAfter = taaIt;
        }
        desc.passes.insert(insertAfter + 1, RenderPassSlot::Present);
    }
}

void insertEditorOverlayAfterPresent(RenderPipelineDesc& desc)
{
    auto presentIt = std::find(desc.passes.begin(), desc.passes.end(),
                               RenderPassSlot::Present);
    if (presentIt != desc.passes.end()) {
        desc.passes.insert(presentIt + 1, RenderPassSlot::EditorOverlay);
    } else {
        desc.passes.push_back(RenderPassSlot::EditorOverlay);
    }
}

} // namespace

RenderPipelineDesc RenderPipelineDesc::makeEditorForward()
{
    RenderPipelineDesc desc = makeForwardWithShadows();
    insertEditorOverlayAfterPresent(desc);
    return desc;
}

RenderPipelineDesc RenderPipelineDesc::makeEditorDeferred()
{
    RenderPipelineDesc desc = makeDeferred();
    insertEditorOverlayAfterPresent(desc);
    return desc;
}

RenderPipelineDesc RenderPipelineDesc::makeDeferred()
{
    // GBuffer/Lighting own opaque Deferred rendering; do not mount a second
    // ForwardOpaque draw. Feature settings decide whether optional stages run.
    return RenderPipelineDesc{{
        RenderPassSlot::Shadow,
        RenderPassSlot::Skybox,
        RenderPassSlot::GBuffer,
        RenderPassSlot::DeferredDecal,
        RenderPassSlot::MotionVector,
        RenderPassSlot::SSAO,
        RenderPassSlot::Lighting,
        RenderPassSlot::SSR,
        RenderPassSlot::DepthHaze,
        RenderPassSlot::Volumetric,
        RenderPassSlot::Transparent,
        // The same camera-overlay 2D stage is available in Deferred. It writes
        // the current Lighting/Haze color producer without entering GBuffer.
        RenderPassSlot::Forward2DOpaque,
        RenderPassSlot::BloomExtract,
        RenderPassSlot::BloomBlur,
        RenderPassSlot::AutoExposure,
        RenderPassSlot::PostProcess,
        RenderPassSlot::TAA,
        RenderPassSlot::FXAA,
        RenderPassSlot::SMAA,
        RenderPassSlot::StylizedOutline,
        RenderPassSlot::ColorGrading,
        RenderPassSlot::ColorStylization,
        RenderPassSlot::TemporalFeedback,
        RenderPassSlot::AnalogDisplay,
        RenderPassSlot::Present,
        RenderPassSlot::UI,
        // Fullscreen attachment overlay on view 250. It is mounted only in
        // Deferred, defaults off, and executes before editor UI (view 255).
        RenderPassSlot::GBufferDebug,
    }, RenderPath::Deferred};
}

bool RenderPipelineDesc::contains(RenderPassSlot slot) const noexcept
{
    for (const RenderPassSlot s : passes) {
        if (s == slot) {
            return true;
        }
    }
    return false;
}

namespace detail {
RenderPipelineDesc resolveRenderPipelineDesc(const RenderPipelineDesc& requested)
{
    RenderPipelineDesc resolved = requested.passes.empty()
        ? RenderPipelineDesc::makeDefault() : requested;
    // Preserve source compatibility for descriptors predating Present.
    // Validation and GPU lifetime changes remain the caller's responsibility.
    ensurePresentAfterPostProcess(resolved);
    return resolved;
}
} // namespace detail
} // namespace ayt::render
