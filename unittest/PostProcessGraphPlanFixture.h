#pragma once

#include "detail/PostProcessGraphPlan.h"
#include "detail/BGFXAdapter.h"
#include <array>

namespace ayt::render::detail {
// Legacy named toggles belong to fixtures, not the product graph interface.
// Existing algorithm tests still exercise the same prepared-contribution path.
struct PostProcessGraphPlanFixtureInput : PostProcessGraphPlanInput {
    bool colorGrading=false,colorStylization=false,stylizedOutline=false;
    bool analogDisplay=false,temporalFeedback=false;
    bool stylizationDepth=false,stylizationNormals=false;
    bgfx::FrameBufferHandle feedbackReadHistory=BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle feedbackWriteTarget=BGFX_INVALID_HANDLE;
};

inline PostProcessGraphPlanResult buildPostProcessGraphPlan(
    FrameGraph& graph,const PostProcessGraphPlanFixtureInput& input)
{
    PostProcessGraphPlanInput prepared=input;
    std::array<PresentationStageFrame,kPresentationStages.size()> stages;
    if(prepared.presentationStages.empty()) {
        for(size_t i=0;i<stages.size();++i) {
            auto& stage=stages[i];const auto& info=kPresentationStages[i];stage.info=&info;
            switch(info.slot) {
            case RenderPassSlot::StylizedOutline:
                stage.enabled=input.stylizedOutline;
                if(input.stylizationDepth) {
                    stage.imports.push_back({FgResourceId::StylizationDepth,
                        BGFXAdapter::isValid(input.gbuffer)?input.gbuffer:input.sceneColor});
                    stage.extraReads.push_back(FgResourceId::StylizationDepth);
                }
                if(input.stylizationNormals) {
                    if(BGFXAdapter::isValid(input.gbuffer))
                        stage.imports.push_back({FgResourceId::GBufferNormal,input.gbuffer});
                    stage.extraReads.push_back(FgResourceId::GBufferNormal);
                }
                break;
            case RenderPassSlot::ColorGrading:stage.enabled=input.colorGrading;break;
            case RenderPassSlot::ColorStylization:stage.enabled=input.colorStylization;break;
            case RenderPassSlot::TemporalFeedback:
                stage.enabled=input.temporalFeedback;stage.historyTarget=input.feedbackWriteTarget;
                stage.imports.push_back({FgResourceId::FeedbackHistory,input.feedbackReadHistory});
                stage.extraReads.push_back(FgResourceId::FeedbackHistory);break;
            case RenderPassSlot::AnalogDisplay:stage.enabled=input.analogDisplay;break;
            default:break;
            }
        }
        prepared.presentationStages=stages;
    }
    return buildPostProcessGraphPlan(graph,prepared);
}
}
