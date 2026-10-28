#include "detail/TemporalFeedbackPass.h"
#include "AYRenderer/TemporalFeedbackShaderSources.h"
#include "detail/PassExecContext.h"
#include "detail/PresentationStage.h"
#include <algorithm>
#include <cmath>
namespace ayt::render::detail {
namespace {
void invalidateFeedbackEntries(RenderResourceBlackboard* bb,ResourceInvalidationReason reason) {
    if(!bb)return;
    bb->invalidate(BlackboardResourceId::FeedbackHistoryRead,reason);
    bb->invalidate(BlackboardResourceId::FeedbackHistoryWrite,reason);
}
}
namespace {
constexpr auto kFeedbackContractReads=std::to_array<RenderPassResourceRead>({
    {RenderPassResourceId::FeedbackHistory,false}});
}
void TemporalFeedbackPass::applyPresentationLook(const ColorStylizationSettings& look,
    BGFXAdapter& adapter,RenderResourceBlackboard* blackboard) {
    setSettings(look);
    if(!active())releaseHistory(adapter,blackboard);
}
void TemporalFeedbackPass::invalidatePresentationHistory(ResourceInvalidationReason reason,
    RenderResourceBlackboard* blackboard) {
    invalidateHistory(reason);invalidateFeedbackEntries(blackboard,reason);
}
void TemporalFeedbackPass::preparePresentationResources(PresentationFrameContext& context,
    PresentationStageFrame& stage) {
    stage.enabled=prepareFrame(context.adapter,context.pool,context.frame,
        context.x,context.y,context.width,context.height,context.blackboard);
    if(!stage.enabled)return;
    stage.historyTarget=writeFbo();
    stage.imports.push_back({FgResourceId::FeedbackHistory,readFbo()});
    stage.extraReads.push_back(FgResourceId::FeedbackHistory);
}
void TemporalFeedbackPass::setSettings(const ColorStylizationSettings& look) noexcept {
    auto s=sanitizeTemporalFeedback(look.temporal);s.enabled &= look.enabled;
    if(s.enabled!=_settings.enabled || s.mode!=_settings.mode || s.holdFrames!=_settings.holdFrames
        || s.tileSize!=_settings.tileSize || s.noiseSeed!=_settings.noiseSeed)
        invalidateHistory(s.enabled?ResourceInvalidationReason::Manual:ResourceInvalidationReason::FeatureDisabled);
    _settings=s;setEnabled(active());
}
void TemporalFeedbackPass::invalidateHistory(ResourceInvalidationReason reason) noexcept {
    _historyValid=false;_produced=false;_frameIndex=0;_reason=reason;
}
void TemporalFeedbackPass::releaseHistory(BGFXAdapter& a,RenderResourceBlackboard* bb,ResourceInvalidationReason reason) {
    for(auto& h:_history){a.destroy(h);h=BGFX_INVALID_HANDLE;}
    _width=_height=0;_readIndex=0;_haveCamera=false;_prepared=false;
    invalidateHistory(reason);
    if(bb)for(auto id:{BlackboardResourceId::FeedbackHistoryRead,BlackboardResourceId::FeedbackHistoryWrite})
        if(bb->find(id))bb->publish(id,BlackboardResourceLifetime::PersistentHistory,
            BGFX_INVALID_HANDLE,BGFX_INVALID_HANDLE,0,0,_generation,false,reason);
}
bool TemporalFeedbackPass::detectCameraCut(const FrameContext& frame) const noexcept {
    if(!_haveCamera)return false;
    const auto delta=frame.cameraPosition-_previousCamera;
    if(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z>16.f)return true;
    for(int i=0;i<16;++i)if(std::abs(frame.projection.ptr()[i]-_previousProjection.ptr()[i])>.02f)return true;
    float alignment=0,lengthA=0,lengthB=0;
    for(int i=0;i<3;++i){const float a=frame.view.row[2][i],b=_previousView.row[2][i];alignment+=a*b;lengthA+=a*a;lengthB+=b*b;}
    return alignment<.5f*std::sqrt(lengthA*lengthB);
}
void TemporalFeedbackPass::publishHistory(BGFXAdapter& a,RenderResourceBlackboard* bb) {
    if(!bb)return;
    const auto texture=[&](bgfx::FrameBufferHandle h){return BGFXAdapter::isValid(h)?a.getFboAttachment(h,0):bgfx::TextureHandle{BGFX_INVALID_HANDLE};};
    bb->publish(BlackboardResourceId::FeedbackHistoryRead,BlackboardResourceLifetime::PersistentHistory,
        readFbo(),texture(readFbo()),_width,_height,_generation,_historyValid,_historyValid?ResourceInvalidationReason::None:_reason);
    bb->publish(BlackboardResourceId::FeedbackHistoryWrite,BlackboardResourceLifetime::PersistentHistory,
        writeFbo(),texture(writeFbo()),_width,_height,_generation,false,ResourceInvalidationReason::AwaitingProducer);
}
bool TemporalFeedbackPass::prepareFrame(BGFXAdapter& a,shader::ShaderResourcePool& pool,
    const FrameContext& frame,uint16_t x,uint16_t y,uint16_t width,uint16_t height,RenderResourceBlackboard* bb) {
    if(_prepared && !_produced)invalidateHistory(ResourceInvalidationReason::ProducerFailure);
    _prepared=false;_produced=false;
    if(!active()){releaseHistory(a,bb);return false;}
    if(!a.isInitialized() || a.isNoopBackend() || !width || !height) {
        invalidateHistory(ResourceInvalidationReason::PreparationFailed);invalidateFeedbackEntries(bb,_reason);return false;
    }
    for(int i=0;i<16;++i)if(!std::isfinite(frame.view.ptr()[i]) || !std::isfinite(frame.projection.ptr()[i])) {
        invalidateHistory(ResourceInvalidationReason::PreparationFailed);invalidateFeedbackEntries(bb,_reason);return false;
    }
    if(!_geometry.ensure(a)) {
        invalidateHistory(ResourceInvalidationReason::PreparationFailed);invalidateFeedbackEntries(bb,_reason);return false;
    }
    if(!_program.isValid()) {
        if(_retryFrames){--_retryFrames;invalidateHistory(ResourceInvalidationReason::PreparationFailed);invalidateFeedbackEntries(bb,_reason);return false;}
        _program=pool.acquire(temporalFeedbackPhoskiaSource(),kTemporalFeedbackCacheKey);
        if(!_program.isValid()) {
            _retryFrames=120;invalidateHistory(ResourceInvalidationReason::PreparationFailed);invalidateFeedbackEntries(bb,_reason);
            rateLimitedEarlyReturn("TemporalFeedbackPass","shader unavailable; retaining presentation source");return false;
        }
    }
    bool ready=true;
    for(const char* name:{"currentColor","previousColor"})ready &= _program.getTextureBinding(name)!=shader::InvalidBinding;
    for(const char* name:{"feedbackParams","feedbackGrid","feedbackOffset"})ready &= _program.getUniformBinding(name)!=shader::InvalidBinding;
    if(!ready){invalidateHistory(ResourceInvalidationReason::PreparationFailed);invalidateFeedbackEntries(bb,_reason);
        rateLimitedEarlyReturn("TemporalFeedbackPass","program binding ABI unavailable");return false;}
    if(_width!=width || _height!=height || !BGFXAdapter::isValid(readFbo()) || !BGFXAdapter::isValid(writeFbo())) {
        releaseHistory(a,nullptr,ResourceInvalidationReason::Resize);
        for(auto& h:_history)h=a.createFrameBuffer(width,height,bgfx::TextureFormat::RGBA8,false);
        if(!BGFXAdapter::isValid(_history[0]) || !BGFXAdapter::isValid(_history[1])) {
            releaseHistory(a,bb,ResourceInvalidationReason::PreparationFailed);return false;
        }
        _width=width;_height=height;if(++_generation==0)++_generation;
        invalidateHistory(ResourceInvalidationReason::ResourceRecreated);
    }
    if(_haveCamera && (x!=_x || y!=_y || detectCameraCut(frame)))invalidateHistory(ResourceInvalidationReason::CameraCut);
    _x=x;_y=y;_previousView=frame.view;_previousProjection=frame.projection;
    _previousCamera=frame.cameraPosition;_haveCamera=true;_prepared=true;
    publishHistory(a,bb);return true;
}
uint32_t TemporalFeedbackPass::execute(PassExecContext& c) {
    if(!active() || !_prepared){rateLimitedEarlyReturn("TemporalFeedbackPass","stage not prepared");return 0;}
    if(c.viewportWidth!=_width || c.viewportHeight!=_height || c.viewportX!=_x || c.viewportY!=_y) {
        invalidateHistory(ResourceInvalidationReason::PreparationFailed);invalidateFeedbackEntries(c.resourceBlackboard,_reason);
        rateLimitedEarlyReturn("TemporalFeedbackPass","viewport differs from prepared history");return 0;
    }
    PresentationStageTargets targets;
    if(!resolveTargets(c,targets)) {
        invalidateHistory(ResourceInvalidationReason::ProducerFailure);invalidateFeedbackEntries(c.resourceBlackboard,_reason);
        rateLimitedEarlyReturn("TemporalFeedbackPass",targets.failure?targets.failure:"presentation unavailable");return 0;
    }
    bool historyAvailable=_historyValid;
    if(c.resourceBlackboard) {
        const auto* entry=c.resourceBlackboard->find(BlackboardResourceId::FeedbackHistoryRead);
        historyAvailable &= entry && entry->contentValid && entry->generation==_generation
            && entry->width==c.viewportWidth && entry->height==c.viewportHeight && entry->framebuffer.idx==readFbo().idx;
        if(_historyValid && !historyAvailable)invalidateHistory(entry?entry->invalidationReason:ResourceInvalidationReason::ProducerFailure);
    }
    auto previous=historyAvailable?c.adapter.getFboAttachment(readFbo(),0):targets.input;
    if(!BGFXAdapter::isValid(previous)) {
        invalidateHistory(ResourceInvalidationReason::ProducerFailure);invalidateFeedbackEntries(c.resourceBlackboard,_reason);return 0;
    }
    const auto texture=[&](const char* name,bgfx::TextureHandle h){const auto b=_program.getTextureBinding(name);_program.setTexture(_program.getTextureStage(b),b,toShaderTexture(h));};
    const auto uniform=[&](const char* name,const float (&v)[4]){_program.setUniform(_program.getUniformBinding(name),v,sizeof(v));};
    const float params[4]={float(_settings.mode),_settings.historyWeight,float(_frameIndex%_settings.holdFrames),float(_settings.holdFrames)};
    const float grid[4]={float(c.viewportWidth),float(c.viewportHeight),float(_settings.tileSize),float(_settings.noiseSeed)};
    const float offset[4]={_settings.historyOffsetX,_settings.historyOffsetY,historyAvailable?1.f:0.f,0};
    texture("currentColor",targets.input);texture("previousColor",previous);
    uniform("feedbackParams",params);uniform("feedbackGrid",grid);uniform("feedbackOffset",offset);
    configureFullscreenPassView(c.adapter,kViewId,targets.target,targets.x,targets.y,c.viewportWidth,c.viewportHeight);
    _geometry.bind(c.adapter);c.adapter.setStateDepthTestAlways();_program.submit({kViewId,0});
    publishOutput(c);
    _readIndex=1-_readIndex;_historyValid=true;_reason=ResourceInvalidationReason::None;_produced=true;++_frameIndex;
    if(c.resourceBlackboard)c.resourceBlackboard->markProduced(BlackboardResourceId::FeedbackHistoryWrite);
    return 1;
}
void TemporalFeedbackPass::finishFrame(RenderResourceBlackboard* bb) {
    if(_prepared && !_produced){invalidateHistory(ResourceInvalidationReason::ProducerFailure);invalidateFeedbackEntries(bb,_reason);}
    _prepared=false;
}
void TemporalFeedbackPass::destroyResources(BGFXAdapter& a) {
    releaseHistory(a,nullptr,ResourceInvalidationReason::Shutdown);_program.reset();_geometry.destroy(a);_retryFrames=0;
}
AY_DEFINE_PRESENTATION_STAGE(TemporalFeedback, TemporalFeedbackPass, temporalFeedbackPhoskiaSource(), kFeedbackContractReads)

} // namespace ayt::render::detail
