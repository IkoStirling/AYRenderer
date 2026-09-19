#include "detail/RenderTargetPool.h"

#include "detail/BGFXAdapter.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace ayt::render::detail
{

RenderTargetPool::RenderTargetPool(BGFXAdapter& adapter) noexcept
    : _adapter(&adapter)
{
}

RenderTargetPool::~RenderTargetPool()
{
    shutdown();
}

void RenderTargetPool::beginFrame()
{
    ++_frameIndex;
    trimToBudget();
}

PooledRenderTargetHandle RenderTargetPool::acquire(const RenderTargetKey& key,
                                                   bool allowBudgetOverflow,
                                                   RenderTargetPoolCategory category)
{
    if (_adapter == nullptr || !_adapter->isInitialized()
        || key.width == 0 || key.height == 0 || key.sampleCount != 1) {
        return {};
    }

    for (uint32_t slot = 0; slot < static_cast<uint32_t>(_entries.size()); ++slot) {
        Entry& entry = _entries[slot];
        if (entry.leased || entry.reusableAfterFrame > _frameIndex
            || !BGFXAdapter::isValid(entry.framebuffer) || !(entry.key == key)
            || entry.category != category) {
            continue;
        }
        entry.leased = true;
        entry.lastUsedFrame = _frameIndex;
        ++entry.generation;
        if (entry.generation == 0) ++entry.generation;
        ++_reuses;
        ++_categoryReuses[static_cast<size_t>(category)];
        return {slot, entry.generation};
    }

    const size_t requestedBytes = estimateBytes(key);
    if (!allowBudgetOverflow) {
        const size_t categoryBudget = categoryBudgetBytes(category);
        if (requestedBytes > _budgetBytes || requestedBytes > categoryBudget) {
            ++_budgetMisses;
            ++_categoryBudgetMisses[static_cast<size_t>(category)];
            return {};
        }
        trimCategoryToBytes(category, categoryBudget - requestedBytes);
        if (allocatedBytes(category) > categoryBudget - requestedBytes) {
            ++_budgetMisses;
            ++_categoryBudgetMisses[static_cast<size_t>(category)];
            return {};
        }
        trimToBytes(_budgetBytes - requestedBytes);
        if (_allocatedBytes > _budgetBytes - requestedBytes) {
            ++_budgetMisses;
            ++_categoryBudgetMisses[static_cast<size_t>(category)];
            return {};
        }
    }

    bgfx::FrameBufferHandle framebuffer = _adapter->createFrameBuffer(
        key.width, key.height, key.colorFormat, key.withDepth, key.pointSampled);
    // A shared pool can be below its byte budget and still exhaust bgfx's
    // finite framebuffer/texture handle tables after several viewport or UI
    // layer resizes. Exact-key lookup cannot reuse differently shaped idle
    // targets, so reclaim one target whose in-flight quarantine has expired
    // and retry once before degrading the requesting pass.
    if (!BGFXAdapter::isValid(framebuffer) && evictOldestIdle()) {
        framebuffer = _adapter->createFrameBuffer(
            key.width, key.height, key.colorFormat, key.withDepth,
            key.pointSampled);
    }
    if (!BGFXAdapter::isValid(framebuffer)) {
        return {};
    }

    uint32_t slot = PooledRenderTargetHandle::kInvalidSlot;
    for (uint32_t i = 0; i < static_cast<uint32_t>(_entries.size()); ++i) {
        if (!BGFXAdapter::isValid(_entries[i].framebuffer) && !_entries[i].leased) {
            slot = i;
            break;
        }
    }
    if (slot == PooledRenderTargetHandle::kInvalidSlot) {
        slot = static_cast<uint32_t>(_entries.size());
        _entries.emplace_back();
    }

    Entry& entry = _entries[slot];
    entry.key = key;
    entry.framebuffer = framebuffer;
    entry.leased = true;
    entry.lastUsedFrame = _frameIndex;
    entry.reusableAfterFrame = _frameIndex;
    entry.estimatedBytes = requestedBytes;
    entry.category = category;
    ++entry.generation;
    if (entry.generation == 0) ++entry.generation;
    _allocatedBytes += entry.estimatedBytes;
    _peakAllocatedBytes = std::max(_peakAllocatedBytes, _allocatedBytes);
    ++_allocations;
    ++_categoryAllocations[static_cast<size_t>(category)];
    return {slot, entry.generation};
}

void RenderTargetPool::release(PooledRenderTargetHandle handle)
{
    Entry* entry = find(handle);
    if (entry == nullptr) return;
    entry->leased = false;
    entry->lastUsedFrame = _frameIndex;
    entry->reusableAfterFrame = _frameIndex + _deferredFrames;
    ++_releases;
}

bool RenderTargetPool::isValid(PooledRenderTargetHandle handle) const noexcept
{
    return find(handle) != nullptr;
}

bool RenderTargetPool::matches(PooledRenderTargetHandle handle,
                               const RenderTargetKey& key) const noexcept
{
    const Entry* entry = find(handle);
    return entry != nullptr && entry->key == key;
}

bgfx::FrameBufferHandle RenderTargetPool::framebuffer(
    PooledRenderTargetHandle handle) const noexcept
{
    const Entry* entry = find(handle);
    return entry != nullptr ? entry->framebuffer
                            : bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
}

bgfx::TextureHandle RenderTargetPool::texture(PooledRenderTargetHandle handle,
                                              uint8_t attachment) const
{
    const Entry* entry = find(handle);
    if (entry == nullptr || _adapter == nullptr) {
        return bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    }
    return _adapter->getFboAttachment(entry->framebuffer, attachment);
}

void RenderTargetPool::setBudgetBytes(size_t bytes)
{
    _budgetBytes = bytes;
    trimToBudget();
}

void RenderTargetPool::setCategoryBudgetBytes(
    RenderTargetPoolCategory category, size_t bytes)
{
    const size_t index = static_cast<size_t>(category);
    if (index >= _categoryBudgets.size()) return;
    _categoryBudgets[index] = bytes;
    trimCategoryToBytes(category, bytes);
}

size_t RenderTargetPool::categoryBudgetBytes(
    RenderTargetPoolCategory category) const noexcept
{
    const size_t index = static_cast<size_t>(category);
    return index < _categoryBudgets.size() ? _categoryBudgets[index] : 0u;
}

RenderTargetPoolStats RenderTargetPool::stats() const noexcept
{
    RenderTargetPoolStats out{};
    out.allocations = _allocations;
    out.reuses = _reuses;
    out.releases = _releases;
    out.evictions = _evictions;
    out.budgetMisses = _budgetMisses;
    out.allocatedBytes = _allocatedBytes;
    out.budgetBytes = _budgetBytes;
    out.peakAllocatedBytes = _peakAllocatedBytes;
    for (const Entry& entry : _entries) {
        if (!BGFXAdapter::isValid(entry.framebuffer)) continue;
        if (entry.leased) ++out.liveLeases;
        else ++out.idleTargets;
        auto& category = out.categories[static_cast<size_t>(entry.category)];
        category.allocatedBytes += entry.estimatedBytes;
        if (entry.leased) ++category.liveLeases;
        else ++category.idleTargets;
    }
    for (size_t index = 0; index < out.categories.size(); ++index) {
        out.categories[index].allocations = _categoryAllocations[index];
        out.categories[index].reuses = _categoryReuses[index];
        out.categories[index].budgetMisses = _categoryBudgetMisses[index];
        out.categories[index].budgetBytes = _categoryBudgets[index];
    }
    return out;
}

void RenderTargetPool::resetStats() noexcept
{
    _allocations = 0;
    _reuses = 0;
    _releases = 0;
    _evictions = 0;
    _budgetMisses = 0;
    _categoryAllocations.fill(0u);
    _categoryReuses.fill(0u);
    _categoryBudgetMisses.fill(0u);
    _peakAllocatedBytes = _allocatedBytes;
}

void RenderTargetPool::reset()
{
    for (Entry& entry : _entries) {
        destroyEntry(entry);
        entry.leased = false;
        entry.reusableAfterFrame = 0;
        ++entry.generation;
        if (entry.generation == 0) ++entry.generation;
    }
    _allocatedBytes = 0;
}

void RenderTargetPool::shutdown()
{
    reset();
    _entries.clear();
    _frameIndex = 0;
}

const RenderTargetPool::Entry* RenderTargetPool::find(
    PooledRenderTargetHandle handle) const noexcept
{
    if (!handle.isValid() || handle.slot >= _entries.size()) return nullptr;
    const Entry& entry = _entries[handle.slot];
    if (!entry.leased || entry.generation != handle.generation
        || !BGFXAdapter::isValid(entry.framebuffer)) {
        return nullptr;
    }
    return &entry;
}

RenderTargetPool::Entry* RenderTargetPool::find(PooledRenderTargetHandle handle) noexcept
{
    return const_cast<Entry*>(std::as_const(*this).find(handle));
}

void RenderTargetPool::trimToBudget()
{
    trimToBytes(_budgetBytes);
}

void RenderTargetPool::trimToBytes(size_t bytes)
{
    while (_allocatedBytes > bytes && evictOldestIdle()) {}
}

bool RenderTargetPool::evictOldestIdle()
{
    Entry* victim = nullptr;
    for (Entry& entry : _entries) {
        if (entry.leased || entry.reusableAfterFrame > _frameIndex
            || !BGFXAdapter::isValid(entry.framebuffer)) {
            continue;
        }
        if (victim == nullptr || entry.lastUsedFrame < victim->lastUsedFrame) {
            victim = &entry;
        }
    }
    if (victim == nullptr) return false;
    destroyEntry(*victim);
    ++_evictions;
    return true;
}

bool RenderTargetPool::evictOldestIdle(RenderTargetPoolCategory category)
{
    Entry* victim = nullptr;
    for (Entry& entry : _entries) {
        if (entry.category != category || entry.leased
            || entry.reusableAfterFrame > _frameIndex
            || !BGFXAdapter::isValid(entry.framebuffer)) {
            continue;
        }
        if (victim == nullptr || entry.lastUsedFrame < victim->lastUsedFrame) {
            victim = &entry;
        }
    }
    if (victim == nullptr) return false;
    destroyEntry(*victim);
    ++_evictions;
    return true;
}

void RenderTargetPool::trimCategoryToBytes(
    RenderTargetPoolCategory category, size_t bytes)
{
    while (allocatedBytes(category) > bytes
           && evictOldestIdle(category)) {}
}

size_t RenderTargetPool::allocatedBytes(
    RenderTargetPoolCategory category) const noexcept
{
    size_t bytes = 0u;
    for (const Entry& entry : _entries) {
        if (entry.category == category
            && BGFXAdapter::isValid(entry.framebuffer)) {
            bytes += entry.estimatedBytes;
        }
    }
    return bytes;
}

void RenderTargetPool::destroyEntry(Entry& entry)
{
    if (!BGFXAdapter::isValid(entry.framebuffer)) return;
    if (_adapter != nullptr && _adapter->isInitialized()) {
        _adapter->destroy(entry.framebuffer);
    }
    entry.framebuffer = BGFX_INVALID_HANDLE;
    _allocatedBytes = entry.estimatedBytes <= _allocatedBytes
        ? _allocatedBytes - entry.estimatedBytes : 0;
    entry.estimatedBytes = 0;
}

size_t RenderTargetPool::estimateBytes(const RenderTargetKey& key) noexcept
{
    size_t colorBytes = 4;
    switch (key.colorFormat) {
    case bgfx::TextureFormat::R8: colorBytes = 1; break;
    case bgfx::TextureFormat::RG8: colorBytes = 2; break;
    case bgfx::TextureFormat::RGBA16F: colorBytes = 8; break;
    case bgfx::TextureFormat::RGBA32F: colorBytes = 16; break;
    default: break;
    }
    const size_t pixelBytes = colorBytes + (key.withDepth ? 4u : 0u);
    return static_cast<size_t>(key.width) * static_cast<size_t>(key.height) * pixelBytes;
}

} // namespace ayt::render::detail
