#pragma once
#include "AYRenderer/ColorStylizationSettings.h"
#include "AYShader/ShaderResource.h"
#include "detail/FullscreenPassGeometry.h"
#include "detail/PresentationPass.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/FrameContext.h"
#include <array>
namespace ayt::render::detail {
class TemporalFeedbackPass final : public PresentationPass {
public:
    TemporalFeedbackPass() : PresentationPass(RenderPassSlot::TemporalFeedback) {}
    static constexpr uint8_t kViewId=presentationStageViewId(RenderPassSlot::TemporalFeedback);
    void setSettings(const ColorStylizationSettings& look) noexcept;
    bool active() const noexcept override {return temporalFeedbackActive(_settings);}
    void applyPresentationLook(const ColorStylizationSettings&, BGFXAdapter&,
        RenderResourceBlackboard*) override;
    void invalidatePresentationHistory(ResourceInvalidationReason,
        RenderResourceBlackboard*) override;
    void finishPresentationFrame(RenderResourceBlackboard* blackboard) override {finishFrame(blackboard);}
    bool prepareFrame(BGFXAdapter& adapter,shader::ShaderResourcePool& pool,
        const FrameContext& frame,uint16_t x,uint16_t y,uint16_t width,uint16_t height,
        RenderResourceBlackboard* blackboard=nullptr);
    uint32_t execute(PassExecContext& context) override;
    void finishFrame(RenderResourceBlackboard* blackboard=nullptr);
    void invalidateHistory(ResourceInvalidationReason reason=ResourceInvalidationReason::Manual) noexcept;
    void releaseHistory(BGFXAdapter& adapter,RenderResourceBlackboard* blackboard=nullptr,
        ResourceInvalidationReason reason=ResourceInvalidationReason::FeatureDisabled);
    void destroyResources(BGFXAdapter& adapter) override;
    bgfx::FrameBufferHandle readFbo() const noexcept {return _history[_readIndex];}
    bgfx::FrameBufferHandle writeFbo() const noexcept {return _history[1-_readIndex];}
    bool historyValid() const noexcept {return _historyValid;}
    bool producedThisFrame() const noexcept {return _produced;}
    uint64_t completedFrames() const noexcept {return _frameIndex;}
    uint32_t generation() const noexcept {return _generation;}
    ResourceInvalidationReason invalidationReason() const noexcept {return _reason;}
private:
    void preparePresentationResources(PresentationFrameContext&,
        PresentationStageFrame&) override;
    TemporalFeedbackSettings _settings;
    FullscreenPassGeometry _geometry;
    shader::ShaderResource _program;
    std::array<bgfx::FrameBufferHandle,2> _history{
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE}};
    uint16_t _width=0,_height=0,_x=0,_y=0,_retryFrames=0;
    uint8_t _readIndex=0;
    uint32_t _generation=0;
    uint64_t _frameIndex=0;
    bool _historyValid=false,_prepared=false,_produced=false,_haveCamera=false;
    ResourceInvalidationReason _reason=ResourceInvalidationReason::FeatureDisabled;
    math::Float4x4 _previousView=math::Float4x4::identity();
    math::Float4x4 _previousProjection=math::Float4x4::identity();
    math::FVector3 _previousCamera{};
    bool detectCameraCut(const FrameContext& frame) const noexcept;
    void publishHistory(BGFXAdapter& adapter,RenderResourceBlackboard* blackboard);
};
} // namespace ayt::render::detail
