#pragma once

#include "AYShader/ShaderResource.h"

#include "detail/FullscreenPassGeometry.h"
#include "detail/RenderPass.h"
#include "detail/RenderResourceBlackboard.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class AutoExposurePass final : public RenderPass {
public:
    static constexpr uint8_t kViewId = 31;

    AutoExposurePass();
    ~AutoExposurePass() override;

    std::string_view name() const override { return "AutoExposure"; }
    uint32_t execute(PassExecContext& ctx) override;

    void setSettings(bool enabled, float keyValue, float minimum,
                     float maximum, float brightenSpeed,
                     float darkenSpeed) noexcept;
    bool prepareFrame(BGFXAdapter& adapter, uint16_t width, uint16_t height);
    void invalidateHistory(ResourceInvalidationReason reason) noexcept;
    void destroyResources(BGFXAdapter& adapter);

    bool historyValid() const noexcept { return _historyValid; }
    ResourceInvalidationReason invalidationReason() const noexcept {
        return _invalidationReason;
    }
    uint32_t generation() const noexcept { return _generation; }
    bgfx::FrameBufferHandle readFbo() const noexcept {
        return _history[_readIndex];
    }
    bgfx::FrameBufferHandle writeFbo() const noexcept {
        return _history[1u - _readIndex];
    }
    bool producedThisFrame() const noexcept { return _producedThisFrame; }
    void resetFrameState() noexcept { _producedThisFrame = false; }

private:
    FullscreenPassGeometry _fullscreen;
    ayt::shader::ShaderResource _program;
    ayt::shader::BindingId _tSceneColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tPreviousExposure = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uExposureRange = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uAdaptation = ayt::shader::InvalidBinding;
    bgfx::FrameBufferHandle _history[2] = {
        BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE};
    uint8_t _readIndex = 0;
    uint32_t _generation = 0;
    bool _enabled = false;
    bool _historyValid = false;
    bool _producedThisFrame = false;
    bool _programAcquireFailed = false;
    float _keyValue = 0.18f;
    float _minimum = 0.25f;
    float _maximum = 4.0f;
    float _brightenSpeed = 2.0f;
    float _darkenSpeed = 1.0f;
    float _lastTimeSeconds = -1.0f;
    ResourceInvalidationReason _invalidationReason =
        ResourceInvalidationReason::FeatureDisabled;

    void ensureProgram(shader::ShaderResourcePool& pool);
};

} // namespace ayt::render::detail
