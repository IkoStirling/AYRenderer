#pragma once

#include "detail/FgResource.h"

#include <bgfx/bgfx.h>

#include <cstdint>

namespace ayt::render::detail
{

// Pure declaration input for the renderer-owned post-process resource graph.
// Stage eligibility is decided by Renderer from runtime/pass capabilities;
// this layer only describes resources, dependencies and presentation order.
struct PostProcessGraphPlanInput {
    uint16_t width = 0;
    uint16_t height = 0;

    bgfx::FrameBufferHandle sceneColor = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle taaWriteTarget = BGFX_INVALID_HANDLE;

    bool ssao = false;
    bool haze = false;
    bool bloomExtract = false;
    bool bloomBlur = false;
    bool finalLdr = false;
    bool taa = false;
    bool fxaa = false;
    bool smaa = false;
    bool colorGrading = false;
    bool smaaIntermediatePointSampled = false;
};

struct PostProcessGraphPlanResult {
    bool compileSucceeded = false;
    FgResourceId hdrSceneSource = FgResourceId::SceneColor;
    FgResourceId antialiasingSource = FgResourceId::FinalLdrColor;
    FgResourceId presentationSource = FgResourceId::FinalLdrColor;
};

// Rebuilds and compiles one frame's graph. No concrete RenderPass is retained
// by the plan, keeping graph policy testable without a renderer/backend.
PostProcessGraphPlanResult buildPostProcessGraphPlan(
    FrameGraph& graph,
    const PostProcessGraphPlanInput& input);

} // namespace ayt::render::detail
