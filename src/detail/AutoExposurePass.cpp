#include "detail/AutoExposurePass.h"

#include "AYRenderer/AutoExposureShaderSources.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/SceneColorPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace ayt::render::detail
{

AutoExposurePass::AutoExposurePass() = default;
AutoExposurePass::~AutoExposurePass() = default;

void AutoExposurePass::setSettings(bool enabled, float keyValue,
                                   float minimum, float maximum,
                                   float brightenSpeed,
                                   float darkenSpeed) noexcept
{
    if (_enabled && !enabled) {
        invalidateHistory(ResourceInvalidationReason::FeatureDisabled);
    }
    _enabled = enabled;
    _keyValue = std::clamp(std::isfinite(keyValue) ? keyValue : 0.18f,
                           0.01f, 2.0f);
    _minimum = std::clamp(std::isfinite(minimum) ? minimum : 0.25f,
                          0.01f, 16.0f);
    _maximum = std::clamp(std::isfinite(maximum) ? maximum : 4.0f,
                          _minimum, 32.0f);
    _brightenSpeed = std::clamp(
        std::isfinite(brightenSpeed) ? brightenSpeed : 2.0f, 0.01f, 20.0f);
    _darkenSpeed = std::clamp(
        std::isfinite(darkenSpeed) ? darkenSpeed : 1.0f, 0.01f, 20.0f);
}

bool AutoExposurePass::prepareFrame(BGFXAdapter& adapter, uint16_t width,
                                    uint16_t height)
{
    resetFrameState();
    if (!_enabled || width == 0 || height == 0 || !adapter.isInitialized()
        || adapter.isNoopBackend()) {
        return false;
    }
    if (BGFXAdapter::isValid(_history[0])
        && BGFXAdapter::isValid(_history[1])) {
        return true;
    }
    if (BGFXAdapter::isValid(_history[0])) {
        adapter.destroy(_history[0]);
    }
    if (BGFXAdapter::isValid(_history[1])) {
        adapter.destroy(_history[1]);
    }
    _history[0] = adapter.createFrameBuffer(
        1, 1, bgfx::TextureFormat::RGBA16F, false);
    _history[1] = adapter.createFrameBuffer(
        1, 1, bgfx::TextureFormat::RGBA16F, false);
    if (!BGFXAdapter::isValid(_history[0])
        || !BGFXAdapter::isValid(_history[1])) {
        if (BGFXAdapter::isValid(_history[0])) adapter.destroy(_history[0]);
        if (BGFXAdapter::isValid(_history[1])) adapter.destroy(_history[1]);
        _history[0] = BGFX_INVALID_HANDLE;
        _history[1] = BGFX_INVALID_HANDLE;
        invalidateHistory(ResourceInvalidationReason::PreparationFailed);
        return false;
    }
    ++_generation;
    _readIndex = 0;
    invalidateHistory(ResourceInvalidationReason::ResourceRecreated);
    return true;
}

uint32_t AutoExposurePass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;
    if (!_enabled || !ctx.adapter.isInitialized()
        || ctx.adapter.isNoopBackend() || ctx.frameGraph == nullptr
        || !BGFXAdapter::isValid(readFbo())
        || !BGFXAdapter::isValid(writeFbo())) {
        return 0;
    }
    const bgfx::FrameBufferHandle sourceFbo = selectSceneColorSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }
    const bgfx::TextureHandle sceneTexture =
        ctx.adapter.getFboAttachment(sourceFbo, 0);
    const bgfx::TextureHandle previousTexture =
        ctx.adapter.getFboAttachment(readFbo(), 0);
    const bgfx::TextureHandle outputTexture =
        ctx.adapter.getFboAttachment(writeFbo(), 0);
    if (!BGFXAdapter::isValid(sceneTexture)
        || !BGFXAdapter::isValid(previousTexture)
        || !BGFXAdapter::isValid(outputTexture)
        || !_fullscreen.ensure(ctx.adapter)) {
        return 0;
    }
    ensureProgram(ctx.pool);
    if (!_program.isValid()
        || _tSceneColor == ayt::shader::InvalidBinding
        || _tPreviousExposure == ayt::shader::InvalidBinding
        || _uExposureRange == ayt::shader::InvalidBinding
        || _uAdaptation == ayt::shader::InvalidBinding) {
        return 0;
    }

    configureFullscreenPassView(ctx.adapter, kViewId, writeFbo(), 0, 0, 1, 1);
    const float dt = _lastTimeSeconds >= 0.0f
        ? std::clamp(ctx.frame.timeSeconds - _lastTimeSeconds, 0.0f, 0.1f)
        : 0.0f;
    _lastTimeSeconds = ctx.frame.timeSeconds;
    const float range[4] = {_keyValue, _minimum, _maximum, 0.0f};
    const float adaptation[4] = {
        1.0f - std::exp(-_brightenSpeed * dt),
        1.0f - std::exp(-_darkenSpeed * dt),
        _historyValid ? 1.0f : 0.0f, 0.0f};

    _fullscreen.bind(ctx.adapter);
    ctx.adapter.setStateDepthTestAlways();
    _program.setTexture(0, _tSceneColor, toShaderTexture(sceneTexture));
    _program.setTexture(0, _tPreviousExposure,
                        toShaderTexture(previousTexture));
    _program.setUniform(_uExposureRange, range, sizeof(range));
    _program.setUniform(_uAdaptation, adaptation, sizeof(adaptation));
    ayt::shader::DrawCallContext submit{};
    submit.viewId = kViewId;
    _program.submit(submit);

    ctx.frameGraph->markProduced(FgResourceId::AutoExposure);
    if (ctx.resourceBlackboard != nullptr) {
        ctx.resourceBlackboard->publishProduced(
            BlackboardResourceId::AutoExposure,
            BlackboardResourceLifetime::PersistentHistory,
            writeFbo(), outputTexture, 1, 1, _generation);
        ctx.resourceBlackboard->markProduced(
            BlackboardResourceId::AutoExposureWrite);
    }
    _historyValid = true;
    _invalidationReason = ResourceInvalidationReason::None;
    _producedThisFrame = true;
    _readIndex = static_cast<uint8_t>(1u - _readIndex);
    return 1;
}

void AutoExposurePass::ensureProgram(shader::ShaderResourcePool& pool)
{
    if (_program.isValid() || _programAcquireFailed) return;
    ayt::shader::ShaderResource program = pool.acquire(
        ayt::render::kAutoExposurePhoskiaSource,
        ayt::render::kAutoExposureCacheKey);
    if (!program.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr, "[AutoExposurePass] shader acquire failed\n");
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[AutoExposurePass]   %s\n", error.c_str());
        }
        return;
    }
    _program = std::move(program);
    _tSceneColor = _program.getTextureBinding("sceneColor");
    _tPreviousExposure = _program.getTextureBinding("previousExposure");
    _uExposureRange = _program.getUniformBinding("exposureRange");
    _uAdaptation = _program.getUniformBinding("adaptation");
}

void AutoExposurePass::invalidateHistory(
    ResourceInvalidationReason reason) noexcept
{
    _historyValid = false;
    _lastTimeSeconds = -1.0f;
    _invalidationReason = reason;
}

void AutoExposurePass::destroyResources(BGFXAdapter& adapter)
{
    _fullscreen.destroy(adapter);
    _program.reset();
    for (bgfx::FrameBufferHandle& fbo : _history) {
        if (BGFXAdapter::isValid(fbo)) adapter.destroy(fbo);
        fbo = BGFX_INVALID_HANDLE;
    }
    _tSceneColor = ayt::shader::InvalidBinding;
    _tPreviousExposure = ayt::shader::InvalidBinding;
    _uExposureRange = ayt::shader::InvalidBinding;
    _uAdaptation = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
    _readIndex = 0;
    _generation = 0;
    invalidateHistory(ResourceInvalidationReason::Shutdown);
}

} // namespace ayt::render::detail
