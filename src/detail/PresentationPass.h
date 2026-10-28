#pragma once

#include "AYRenderer/ColorStylizationSettings.h"
#include "detail/PresentationStagePlan.h"
#include "detail/RenderPass.h"
#include "detail/RenderResourceBlackboard.h"

#include <span>
#include <vector>

namespace ayt::render::detail {

class RenderPipeline;
struct PresentationStageTargets;

struct PresentationFrameContext {
    BGFXAdapter& adapter;
    shader::ShaderResourcePool& pool;
    const FrameContext& frame;
    uint16_t x = 0, y = 0, width = 0, height = 0;
    bool colorChainEnabled = false;
    bgfx::FrameBufferHandle sceneColor = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle gbuffer = BGFX_INVALID_HANDLE;
    RenderResourceBlackboard* blackboard = nullptr;
};

// Internal post-AA extension boundary, not a second scheduler or gameplay API.
// Ordinary stages implement active/execute/cleanup. Resource/history stages
// additionally override the relevant hooks; Renderer has no per-effect branches.
class PresentationPass : public RenderPass {
public:
    explicit PresentationPass(RenderPassSlot slot);
    // Internal extension/fixture descriptor must outlive the pass and every
    // prepared frame that references it.
    explicit PresentationPass(const PresentationStageInfo& info) : _info(&info) {}
    const PresentationStageInfo& presentationInfo() const noexcept { return *_info; }
    std::string_view name() const override { return _info->name; }
    virtual bool active() const noexcept = 0;
    void destroyResources(BGFXAdapter& adapter) override = 0;
    virtual void applyPresentationLook(const ColorStylizationSettings&,
        BGFXAdapter&, RenderResourceBlackboard*) {}
    virtual void invalidatePresentationHistory(ResourceInvalidationReason,
        RenderResourceBlackboard*) {}
    virtual void finishPresentationFrame(RenderResourceBlackboard*) {}

    PresentationStageFrame preparePresentationFrame(PresentationFrameContext& context);
    void preparePresentationFrame(PresentationFrameContext& context, PresentationStageFrame& result);

protected:
    bool resolveTargets(PassExecContext& context, PresentationStageTargets& targets) const;
    void publishOutput(PassExecContext& context) const;
    virtual void preparePresentationResources(PresentationFrameContext&,
        PresentationStageFrame&) {}

private:
    const PresentationStageInfo* _info;
};

std::vector<PresentationStageFrame> preparePresentationStages(
    RenderPipeline& pipeline, PresentationFrameContext& context);
void preparePresentationStages(RenderPipeline& pipeline, PresentationFrameContext& context,
    std::vector<PresentationStageFrame>& storage);
void finishPresentationStages(RenderPipeline& pipeline, RenderResourceBlackboard* blackboard);
void configurePresentationLook(RenderPipeline& pipeline, const ColorStylizationSettings& look,
    BGFXAdapter& adapter, RenderResourceBlackboard* blackboard);
void invalidatePresentationHistories(RenderPipeline& pipeline,
    ResourceInvalidationReason reason, RenderResourceBlackboard* blackboard);
void destroyPresentationResources(RenderPipeline& pipeline, BGFXAdapter& adapter);


} // namespace ayt::render::detail
