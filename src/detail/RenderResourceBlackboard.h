#pragma once

#include <bgfx/bgfx.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayt::render::detail
{

// Renderer-internal names. These are intentionally separate from
// FgResourceId: FrameGraph describes one compiled frame, while the blackboard
// also tracks persistent state across frame boundaries.
enum class BlackboardResourceId : uint8_t {
    MotionVectors = 0,
    TaaHistoryRead,
    TaaHistoryWrite,
    GBufferAlbedo,
    GBufferNormal,
    GBufferWorldPosition,
    GBufferMaterial,
    GBufferDepth,
    SsaoOcclusion,
    DepthHazeColor,
    LightingColor,
    SkyboxColor,
    BloomBright,
    BloomBlurA,
    BloomBlurB,
    Count,
};

enum class BlackboardResourceLifetime : uint8_t {
    External = 0,
    Transient,
    PersistentHistory,
};

enum class ResourceInvalidationReason : uint8_t {
    None = 0,
    AwaitingProducer,
    FeatureDisabled,
    PreparationFailed,
    CameraCut,
    ResourceRecreated,
    Resize,
    PipelineRebuild,
    BackendReset,
    ProducerFailure,
    Shutdown,
    Manual,
};

struct BlackboardResourceEntry final {
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    BlackboardResourceLifetime lifetime =
        BlackboardResourceLifetime::External;
    ResourceInvalidationReason invalidationReason =
        ResourceInvalidationReason::AwaitingProducer;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t generation = 0;
    uint64_t publishedFrame = 0;
    uint64_t producedFrame = 0;
    bool declared = false;
    bool available = false;
    bool contentValid = false;
    bool producedThisFrame = false;
};

struct BlackboardGBufferView final {
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle albedo = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle normal = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle worldPosition = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle material = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depth = BGFX_INVALID_HANDLE;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t generation = 0;
};

// Read-mostly per-renderer registry. It does not own GPU handles in R6-4;
// pass/FrameGraph owners publish borrowed handles and remain responsible for
// destruction. The registry is the single source for temporal validity,
// coherent GBuffer attachment groups, and current-frame production state.
class RenderResourceBlackboard final {
public:
    void beginFrame() noexcept;

    void publish(BlackboardResourceId id,
                 BlackboardResourceLifetime lifetime,
                 bgfx::FrameBufferHandle framebuffer,
                 bgfx::TextureHandle texture,
                 uint16_t width,
                 uint16_t height,
                 uint32_t generation,
                 bool contentValid,
                 ResourceInvalidationReason invalidReason =
                     ResourceInvalidationReason::AwaitingProducer) noexcept;

    void publishProduced(BlackboardResourceId id,
                         BlackboardResourceLifetime lifetime,
                         bgfx::FrameBufferHandle framebuffer,
                         bgfx::TextureHandle texture,
                         uint16_t width,
                         uint16_t height,
                         uint32_t generation) noexcept;

    void publishGBuffer(bgfx::FrameBufferHandle framebuffer,
                        bgfx::TextureHandle albedo,
                        bgfx::TextureHandle normal,
                        bgfx::TextureHandle worldPosition,
                        bgfx::TextureHandle material,
                        bgfx::TextureHandle depth,
                        uint16_t width,
                        uint16_t height,
                        uint32_t generation,
                        bool produced) noexcept;

    void markProduced(BlackboardResourceId id) noexcept;
    void invalidate(BlackboardResourceId id,
                    ResourceInvalidationReason reason) noexcept;
    void invalidateTemporal(ResourceInvalidationReason reason) noexcept;
    void invalidateFrameOutputs(ResourceInvalidationReason reason) noexcept;
    void clear(ResourceInvalidationReason reason) noexcept;

    const BlackboardResourceEntry* find(
        BlackboardResourceId id) const noexcept;
    const BlackboardResourceEntry* findProduced(
        BlackboardResourceId id) const noexcept;
    bool resolveGBuffer(BlackboardGBufferView& view) const noexcept;
    bool producedThisFrame(BlackboardResourceId id) const noexcept;
    uint64_t frameNumber() const noexcept { return _frameNumber; }

private:
    static constexpr size_t kResourceCount =
        static_cast<size_t>(BlackboardResourceId::Count);
    std::array<BlackboardResourceEntry, kResourceCount> _entries{};
    uint64_t _frameNumber = 0;
};

const char* blackboardResourceName(BlackboardResourceId id) noexcept;
const char* resourceInvalidationReasonName(
    ResourceInvalidationReason reason) noexcept;

} // namespace ayt::render::detail
