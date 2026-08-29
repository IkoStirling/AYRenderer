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

PooledRenderTargetHandle RenderTargetPool::acquire(const RenderTargetKey& key)
{
    if (_adapter == nullptr || !_adapter->isInitialized()
        || key.width == 0 || key.height == 0 || key.sampleCount != 1) {
        return {};
    }

    for (uint32_t slot = 0; slot < static_cast<uint32_t>(_entries.size()); ++slot) {
        Entry& entry = _entries[slot];
        if (entry.leased || entry.reusableAfterFrame > _frameIndex
            || !BGFXAdapter::isValid(entry.framebuffer) || !(entry.key == key)) {
            continue;
        }
        entry.leased = true;
        entry.lastUsedFrame = _frameIndex;
        ++entry.generation;
        if (entry.generation == 0) ++entry.generation;
        ++_reuses;
        return {slot, entry.generation};
    }

    bgfx::FrameBufferHandle framebuffer = _adapter->createFrameBuffer(
        key.width, key.height, key.colorFormat, key.withDepth);
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
    entry.estimatedBytes = estimateBytes(key);
    ++entry.generation;
    if (entry.generation == 0) ++entry.generation;
    _allocatedBytes += entry.estimatedBytes;
    ++_allocations;
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

RenderTargetPoolStats RenderTargetPool::stats() const noexcept
{
    RenderTargetPoolStats out{};
    out.allocations = _allocations;
    out.reuses = _reuses;
    out.releases = _releases;
    out.evictions = _evictions;
    out.allocatedBytes = _allocatedBytes;
    out.budgetBytes = _budgetBytes;
    for (const Entry& entry : _entries) {
        if (!BGFXAdapter::isValid(entry.framebuffer)) continue;
        if (entry.leased) ++out.liveLeases;
        else ++out.idleTargets;
    }
    return out;
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
    while (_allocatedBytes > _budgetBytes) {
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
        if (victim == nullptr) break;
        destroyEntry(*victim);
        ++_evictions;
    }
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
