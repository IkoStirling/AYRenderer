#pragma once

#include <bgfx/bgfx.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayt::render::detail
{

class BGFXAdapter;

struct RenderTargetKey {
    uint16_t width = 0;
    uint16_t height = 0;
    bgfx::TextureFormat::Enum colorFormat = bgfx::TextureFormat::RGBA8;
    bool withDepth = false;
    uint8_t sampleCount = 1;
    bool pointSampled = false;

    bool operator==(const RenderTargetKey& other) const noexcept
    {
        return width == other.width && height == other.height
            && colorFormat == other.colorFormat
            && withDepth == other.withDepth
            && sampleCount == other.sampleCount
            && pointSampled == other.pointSampled;
    }
};

struct PooledRenderTargetHandle {
    static constexpr uint32_t kInvalidSlot = UINT32_MAX;

    uint32_t slot = kInvalidSlot;
    uint32_t generation = 0;

    bool isValid() const noexcept { return slot != kInvalidSlot; }
};

struct RenderTargetPoolStats {
    uint32_t allocations = 0;
    uint32_t reuses = 0;
    uint32_t releases = 0;
    uint32_t evictions = 0;
    uint32_t budgetMisses = 0;
    uint32_t liveLeases = 0;
    uint32_t idleTargets = 0;
    size_t allocatedBytes = 0;
    size_t budgetBytes = 0;
    size_t peakAllocatedBytes = 0;
};

// Renderer-wide pool for transient and retained framebuffer targets.
//
// A lease is generation checked, so a released handle can never alias a
// later acquisition of the same slot. Released targets remain quarantined
// for `deferredFrames` before reuse/destruction; this is deliberately longer
// than the CPU frame that recorded their last bgfx submission.
class RenderTargetPool final {
public:
    explicit RenderTargetPool(BGFXAdapter& adapter) noexcept;
    ~RenderTargetPool();

    RenderTargetPool(const RenderTargetPool&) = delete;
    RenderTargetPool& operator=(const RenderTargetPool&) = delete;

    void beginFrame();

    // `allowBudgetOverflow=true` preserves the FrameGraph's historical soft
    // budget. Retained UI uses false so pressure can degrade to immediate
    // rendering instead of growing GPU memory without bound.
    PooledRenderTargetHandle acquire(const RenderTargetKey& key,
                                     bool allowBudgetOverflow = true);
    void release(PooledRenderTargetHandle handle);

    bool isValid(PooledRenderTargetHandle handle) const noexcept;
    bool matches(PooledRenderTargetHandle handle,
                 const RenderTargetKey& key) const noexcept;
    bgfx::FrameBufferHandle framebuffer(PooledRenderTargetHandle handle) const noexcept;
    bgfx::TextureHandle texture(PooledRenderTargetHandle handle,
                                uint8_t attachment = 0) const;

    void setBudgetBytes(size_t bytes);
    size_t budgetBytes() const noexcept { return _budgetBytes; }
    void setDeferredFrames(uint32_t frames) noexcept { _deferredFrames = frames; }
    uint32_t deferredFrames() const noexcept { return _deferredFrames; }

    RenderTargetPoolStats stats() const noexcept;
    void resetStats() noexcept;

    // Device reset invalidates every lease immediately. shutdown additionally
    // drops the slot table; both are idempotent.
    void reset();
    void shutdown();

private:
    struct Entry {
        RenderTargetKey key{};
        bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
        uint32_t generation = 0;
        uint64_t lastUsedFrame = 0;
        uint64_t reusableAfterFrame = 0;
        size_t estimatedBytes = 0;
        bool leased = false;
    };

    const Entry* find(PooledRenderTargetHandle handle) const noexcept;
    Entry* find(PooledRenderTargetHandle handle) noexcept;
    bool evictOldestIdle();
    void trimToBudget();
    void trimToBytes(size_t bytes);
    void destroyEntry(Entry& entry);
    static size_t estimateBytes(const RenderTargetKey& key) noexcept;

    BGFXAdapter* _adapter = nullptr;
    std::vector<Entry> _entries;
    uint64_t _frameIndex = 0;
    size_t _budgetBytes = 256u * 1024u * 1024u;
    size_t _allocatedBytes = 0;
    size_t _peakAllocatedBytes = 0;
    uint32_t _deferredFrames = 2;
    uint32_t _allocations = 0;
    uint32_t _reuses = 0;
    uint32_t _releases = 0;
    uint32_t _evictions = 0;
    uint32_t _budgetMisses = 0;
};

} // namespace ayt::render::detail
