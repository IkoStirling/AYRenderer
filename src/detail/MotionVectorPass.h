#pragma once

#include "detail/RenderPass.h"

#include "AYMath/MathTypes.h"
#include "AYShader/ShaderResource.h"

#include <bgfx/bgfx.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ayt::render::detail
{

struct MotionHistorySnapshot final {
    ayt::math::Float4x4 world = ayt::math::Float4x4::identity();
    std::vector<ayt::math::Float4x4> bones;
    uint64_t lastSeenFrame = 0;
};

// Render-frame history keyed by stable host object identity + mesh identity.
// The mesh component of the key lets one entity submit multiple meshes while
// all submeshes of one mesh still share one previous pose.
class MotionHistoryCache final {
public:
    const MotionHistorySnapshot* find(uint64_t objectId,
                                      uint64_t meshId,
                                      uint32_t skeletonJointCount,
                                      uint64_t currentFrame) const noexcept;

    void commit(uint64_t objectId,
                uint64_t meshId,
                const ayt::math::Float4x4& world,
                const ayt::math::Float4x4* bones,
                uint32_t skeletonJointCount,
                uint64_t currentFrame);
    void pruneBefore(uint64_t oldestFrame);
    void clear() noexcept { _entries.clear(); }
    std::size_t size() const noexcept { return _entries.size(); }

private:
    struct Key final {
        uint64_t objectId = 0;
        uint64_t meshId = 0;
        bool operator==(const Key& rhs) const noexcept {
            return objectId == rhs.objectId && meshId == rhs.meshId;
        }
    };
    struct KeyHash final {
        std::size_t operator()(const Key& key) const noexcept;
    };
    std::unordered_map<Key, MotionHistorySnapshot, KeyHash> _entries;
};

// Deferred temporal surface pass. It owns one full-resolution RGBA16F
// texture: xy = current-minus-previous UV excluding projection jitter,
// z = the same surface's previous NDC depth.
// w = 0 for invalid history, otherwise 1 + min(fwidth(previousDepth), 1).
// The primitive-local footprint lets TAA distinguish depth slope from a cut.
// Clear/missing draws are invalid, never implicitly stationary.
// texture, borrows GBuffer depth through a non-owning FBO shell, and replays
// only opaque 3D draws. It is dormant unless a temporal consumer requests it.
class MotionVectorPass final : public RenderPass {
public:
    // View 3 is also the Forward transparent view. The two uses are mutually
    // exclusive: MotionVector is Deferred-only and executes after GBuffer 7.
    static constexpr uint8_t kMotionVectorViewId = 3;
    static constexpr bgfx::TextureFormat::Enum kVelocityFormat =
        bgfx::TextureFormat::RGBA16F;
    static constexpr uint64_t kHistoryRetentionFrames = 120;

    std::string_view name() const override { return "MotionVector"; }
    uint32_t execute(PassExecContext& ctx) override;

    void setOutputSize(uint16_t width, uint16_t height) noexcept {
        _requestedWidth = width;
        _requestedHeight = height;
    }
    void setRequestedThisFrame(bool requested) noexcept;
    void setProjectionJitter(float xUv, float yUv) noexcept {
        _currentJitterX = xUv;
        _currentJitterY = yUv;
    }
    void resetFrameState() noexcept { _producedThisFrame = false; }
    void invalidateHistory() noexcept;
    void destroyResources(BGFXAdapter& adapter);

    bool producedThisFrame() const noexcept { return _producedThisFrame; }
    bool requestedThisFrame() const noexcept { return _requestedThisFrame; }
    bool hasPreviousFrame() const noexcept { return _hasPreviousFrame; }
    bgfx::TextureHandle velocityTexture() const noexcept {
        return _velocityTexture;
    }
    bgfx::FrameBufferHandle velocityFbo() const noexcept {
        return _velocityFbo;
    }
    uint16_t width() const noexcept { return _allocatedWidth; }
    uint16_t height() const noexcept { return _allocatedHeight; }
    uint32_t targetGeneration() const noexcept { return _targetGeneration; }
    const MotionHistoryCache& historyForTests() const noexcept {
        return _history;
    }

private:
    void ensureResources(BGFXAdapter& adapter,
                         bgfx::TextureHandle gbufferDepth);
    void destroyTarget(BGFXAdapter& adapter);
    void ensurePrograms(ayt::shader::ShaderResourcePool& pool);
    bool programsReady() const noexcept;

    ayt::shader::ShaderResource _opaqueProgram;
    ayt::shader::ShaderResource _cutoutProgram;
    bool _opaqueAcquireFailed = false;
    bool _cutoutAcquireFailed = false;

    bgfx::TextureHandle _velocityTexture = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle _velocityFbo = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle _borrowedDepth = BGFX_INVALID_HANDLE;
    uint16_t _requestedWidth = 0;
    uint16_t _requestedHeight = 0;
    uint16_t _allocatedWidth = 0;
    uint16_t _allocatedHeight = 0;
    uint32_t _targetGeneration = 0;

    MotionHistoryCache _history;
    ayt::math::Float4x4 _previousViewProjection =
        ayt::math::Float4x4::identity();
    uint64_t _frameSerial = 0;
    float _currentJitterX = 0.0f;
    float _currentJitterY = 0.0f;
    float _previousJitterX = 0.0f;
    float _previousJitterY = 0.0f;
    bool _requestedThisFrame = false;
    bool _wasRequestedLastFrame = false;
    bool _producedThisFrame = false;
    bool _hasPreviousFrame = false;
    bool _firstDispatchLogged = false;
};

extern const char* const kMotionVectorCacheKeyCStr;
extern const char* const kMotionVectorCutoutCacheKeyCStr;
const char* motionVectorPhoskiaSourceForTests() noexcept;
const char* motionVectorCutoutPhoskiaSourceForTests() noexcept;

} // namespace ayt::render::detail
