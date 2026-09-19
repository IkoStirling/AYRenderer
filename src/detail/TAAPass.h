#pragma once

#include "detail/FullscreenPassGeometry.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYMath/MathTypes.h"
#include "AYShader/ShaderResource.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

struct TaaJitter final {
    float x = 0.0f;
    float y = 0.0f;
};

// Deterministic eight-sample Halton(2,3) sequence centered on the pixel.
TaaJitter taaHaltonJitter(uint32_t sampleIndex) noexcept;
float taaJitterRampScale(uint32_t stableFrameCount) noexcept;
ayt::math::Float4x4 taaApplyProjectionJitter(
    const ayt::math::Float4x4& projection,
    TaaJitter jitterPixels,
    uint16_t width,
    uint16_t height) noexcept;

// Deferred temporal AA. MotionVectorPass supplies rigid and skinned velocity;
// GBuffer world-position reprojection remains a fail-soft fallback for custom
// pipelines or shader/target allocation failures.
class TAAPass final : public RenderPass {
public:
    static constexpr uint8_t kTaaViewId = 5;
    static constexpr uint32_t kJitterSampleCount = 8;
    static constexpr uint32_t kJitterRampFrameCount = 4;
    // Editor viewports are often substantially smaller than the host window.
    // Keep the Halton footprint inside roughly one quarter pixel per axis;
    // larger offsets were visible whenever history had to be rejected.
    static constexpr float kJitterSpread = 0.50f;
    static constexpr float kStaticHistoryWeight = 0.92f;
    static constexpr float kMovingHistoryWeight = 0.65f;
    static constexpr float kNeighborhoodExpansion = 0.025f;
    static constexpr float kHistoryDepthTolerance = 0.0025f;

    std::string_view name() const override { return "TAA"; }
    uint32_t execute(PassExecContext& ctx) override;

    // Called before scene submission. Jitter is exposed only when every
    // resolve dependency is ready, so shader/FBO failures cannot leave the
    // renderer in a jitter-without-resolve state.
    bool prepareFrame(BGFXAdapter& adapter,
                      ayt::shader::ShaderResourcePool& pool,
                      const ayt::math::Float4x4& view,
                      const ayt::math::Float4x4& projection,
                      const ayt::math::FVector3& cameraPosition,
                      uint16_t width,
                      uint16_t height);

    const ayt::math::Float4x4& jitteredProjection() const noexcept {
        return _jitteredProjection;
    }
    bgfx::FrameBufferHandle writeHistoryFbo() const noexcept;
    bool historyValid() const noexcept { return _historyValid; }
    bool preparedThisFrame() const noexcept { return _preparedThisFrame; }
    uint32_t jitterSampleIndex() const noexcept { return _jitterSampleIndex; }
    TaaJitter currentJitter() const noexcept { return _currentJitter; }

    void invalidateHistory() noexcept;
    void destroyResources(BGFXAdapter& adapter);

private:
    bool ensureHistory(BGFXAdapter& adapter, uint16_t width, uint16_t height);
    void destroyHistory(BGFXAdapter& adapter);
    void ensureProgram(ayt::shader::ShaderResourcePool& pool);
    bool isReady() const noexcept;
    bool detectCameraCut(const ayt::math::Float4x4& view,
                         const ayt::math::Float4x4& projection,
                         const ayt::math::FVector3& cameraPosition) const noexcept;

    FullscreenPassGeometry _geometry;
    ayt::shader::ShaderResource _program;
    ayt::shader::BindingId _tCurrentColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tHistoryColor = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tWorldPosition = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tGeometryData = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _tMotionVectors = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uTaaMetrics = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uTaaParams = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uTaaJitter = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uTaaDepthParams = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uCurrentViewProjection = ayt::shader::InvalidBinding;
    ayt::shader::BindingId _uPreviousViewProjection = ayt::shader::InvalidBinding;

    std::array<bgfx::FrameBufferHandle, 2> _history = {
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},
        bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE},
    };
    uint16_t _historyWidth = 0;
    uint16_t _historyHeight = 0;
    uint8_t _readHistoryIndex = 0;
    uint8_t _writeHistoryIndex = 1;
    uint32_t _jitterSampleIndex = 0;
    uint32_t _stableFrameCount = 0;
    bool _historyValid = false;
    bool _preparedThisFrame = false;
    bool _hasPreviousCamera = false;
    bool _backgroundHistoryValid = false;
    TaaJitter _currentJitter{};
    TaaJitter _previousJitter{};

    ayt::math::Float4x4 _jitteredProjection =
        ayt::math::Float4x4::identity();
    ayt::math::Float4x4 _currentViewProjection =
        ayt::math::Float4x4::identity();
    ayt::math::Float4x4 _currentBaseView =
        ayt::math::Float4x4::identity();
    ayt::math::Float4x4 _currentBaseProjection =
        ayt::math::Float4x4::identity();
    ayt::math::FVector3 _currentCameraPosition{};
    ayt::math::Float4x4 _previousViewProjection =
        ayt::math::Float4x4::identity();
    ayt::math::Float4x4 _previousBaseView =
        ayt::math::Float4x4::identity();
    ayt::math::Float4x4 _previousBaseProjection =
        ayt::math::Float4x4::identity();
    ayt::math::FVector3 _previousCameraPosition{};

    uint16_t _programRetryFrames = 0;
    bool _firstDispatchLogged = false;
};

extern const char* const kTaaCacheKeyCStr;
const char* taaPhoskiaSourceForTests() noexcept;

} // namespace ayt::render::detail
