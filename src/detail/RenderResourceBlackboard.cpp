#include "detail/RenderResourceBlackboard.h"

#include <limits>

namespace ayt::render::detail
{
namespace
{

size_t indexOf(BlackboardResourceId id) noexcept
{
    return static_cast<size_t>(id);
}

bool handleAvailable(bgfx::FrameBufferHandle framebuffer,
                     bgfx::TextureHandle texture) noexcept
{
    return bgfx::isValid(framebuffer) || bgfx::isValid(texture);
}

} // namespace

void RenderResourceBlackboard::beginFrame() noexcept
{
    if (_frameNumber == std::numeric_limits<uint64_t>::max()) {
        _frameNumber = 1;
    } else {
        ++_frameNumber;
    }
    for (BlackboardResourceEntry& entry : _entries) {
        entry.producedThisFrame = false;
        if (entry.declared
            && entry.lifetime !=
                   BlackboardResourceLifetime::PersistentHistory) {
            entry.contentValid = false;
            entry.invalidationReason =
                ResourceInvalidationReason::AwaitingProducer;
        }
    }
}

void RenderResourceBlackboard::publish(
    BlackboardResourceId id,
    BlackboardResourceLifetime lifetime,
    bgfx::FrameBufferHandle framebuffer,
    bgfx::TextureHandle texture,
    uint16_t width,
    uint16_t height,
    uint32_t generation,
    bool contentValid,
    ResourceInvalidationReason invalidReason) noexcept
{
    const size_t index = indexOf(id);
    if (index >= _entries.size()) {
        return;
    }
    BlackboardResourceEntry& entry = _entries[index];
    entry.framebuffer = framebuffer;
    entry.texture = texture;
    entry.lifetime = lifetime;
    entry.width = width;
    entry.height = height;
    entry.generation = generation;
    entry.publishedFrame = _frameNumber;
    entry.declared = true;
    entry.available = handleAvailable(framebuffer, texture);
    entry.contentValid = contentValid && entry.available;
    entry.producedThisFrame = false;
    entry.invalidationReason = entry.contentValid
        ? ResourceInvalidationReason::None : invalidReason;
}

void RenderResourceBlackboard::markProduced(BlackboardResourceId id) noexcept
{
    const size_t index = indexOf(id);
    if (index >= _entries.size()) {
        return;
    }
    BlackboardResourceEntry& entry = _entries[index];
    if (!entry.declared || !entry.available) {
        return;
    }
    entry.producedFrame = _frameNumber;
    entry.producedThisFrame = true;
    entry.contentValid = true;
    entry.invalidationReason = ResourceInvalidationReason::None;
}

void RenderResourceBlackboard::invalidate(
    BlackboardResourceId id,
    ResourceInvalidationReason reason) noexcept
{
    const size_t index = indexOf(id);
    if (index >= _entries.size()) {
        return;
    }
    BlackboardResourceEntry& entry = _entries[index];
    entry.contentValid = false;
    entry.producedThisFrame = false;
    entry.invalidationReason = reason;
}

void RenderResourceBlackboard::invalidateTemporal(
    ResourceInvalidationReason reason) noexcept
{
    invalidate(BlackboardResourceId::MotionVectors, reason);
    invalidate(BlackboardResourceId::TaaHistoryRead, reason);
    invalidate(BlackboardResourceId::TaaHistoryWrite, reason);
}

void RenderResourceBlackboard::clear(
    ResourceInvalidationReason reason) noexcept
{
    for (BlackboardResourceEntry& entry : _entries) {
        entry = {};
        entry.invalidationReason = reason;
    }
}

const BlackboardResourceEntry* RenderResourceBlackboard::find(
    BlackboardResourceId id) const noexcept
{
    const size_t index = indexOf(id);
    if (index >= _entries.size() || !_entries[index].declared) {
        return nullptr;
    }
    return &_entries[index];
}

bool RenderResourceBlackboard::producedThisFrame(
    BlackboardResourceId id) const noexcept
{
    const BlackboardResourceEntry* entry = find(id);
    return entry != nullptr && entry->producedThisFrame
        && entry->producedFrame == _frameNumber;
}

const char* blackboardResourceName(BlackboardResourceId id) noexcept
{
    switch (id) {
    case BlackboardResourceId::MotionVectors: return "MotionVectors";
    case BlackboardResourceId::TaaHistoryRead: return "TaaHistoryRead";
    case BlackboardResourceId::TaaHistoryWrite: return "TaaHistoryWrite";
    case BlackboardResourceId::Count: break;
    }
    return "Unknown";
}

const char* resourceInvalidationReasonName(
    ResourceInvalidationReason reason) noexcept
{
    switch (reason) {
    case ResourceInvalidationReason::None: return "none";
    case ResourceInvalidationReason::AwaitingProducer: return "awaiting-producer";
    case ResourceInvalidationReason::FeatureDisabled: return "feature-disabled";
    case ResourceInvalidationReason::PreparationFailed: return "preparation-failed";
    case ResourceInvalidationReason::CameraCut: return "camera-cut";
    case ResourceInvalidationReason::ResourceRecreated: return "resource-recreated";
    case ResourceInvalidationReason::Resize: return "resize";
    case ResourceInvalidationReason::PipelineRebuild: return "pipeline-rebuild";
    case ResourceInvalidationReason::BackendReset: return "backend-reset";
    case ResourceInvalidationReason::ProducerFailure: return "producer-failure";
    case ResourceInvalidationReason::Shutdown: return "shutdown";
    case ResourceInvalidationReason::Manual: return "manual";
    }
    return "unknown";
}

} // namespace ayt::render::detail
