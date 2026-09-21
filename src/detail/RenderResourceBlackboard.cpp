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

void RenderResourceBlackboard::publishProduced(
    BlackboardResourceId id,
    BlackboardResourceLifetime lifetime,
    bgfx::FrameBufferHandle framebuffer,
    bgfx::TextureHandle texture,
    uint16_t width,
    uint16_t height,
    uint32_t generation) noexcept
{
    publish(id, lifetime, framebuffer, texture, width, height, generation,
            false, ResourceInvalidationReason::AwaitingProducer);
    markProduced(id);
}

void RenderResourceBlackboard::publishGBuffer(
    bgfx::FrameBufferHandle framebuffer,
    bgfx::TextureHandle albedo,
    bgfx::TextureHandle normal,
    bgfx::TextureHandle worldPosition,
    bgfx::TextureHandle material,
    bgfx::TextureHandle depth,
    uint16_t width,
    uint16_t height,
    uint32_t generation,
    bool produced) noexcept
{
    const struct Resource final {
        BlackboardResourceId id;
        bgfx::TextureHandle texture;
    } resources[] = {
        {BlackboardResourceId::GBufferAlbedo, albedo},
        {BlackboardResourceId::GBufferNormal, normal},
        {BlackboardResourceId::GBufferWorldPosition, worldPosition},
        {BlackboardResourceId::GBufferMaterial, material},
        {BlackboardResourceId::GBufferDepth, depth},
    };
    for (const Resource& resource : resources) {
        publish(resource.id, BlackboardResourceLifetime::External,
                framebuffer, resource.texture, width, height, generation,
                false, ResourceInvalidationReason::AwaitingProducer);
        if (produced) {
            markProduced(resource.id);
        }
    }
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

void RenderResourceBlackboard::invalidateFrameOutputs(
    ResourceInvalidationReason reason) noexcept
{
    for (const BlackboardResourceId id : {
             BlackboardResourceId::MotionVectors,
             BlackboardResourceId::GBufferAlbedo,
             BlackboardResourceId::GBufferNormal,
             BlackboardResourceId::GBufferWorldPosition,
             BlackboardResourceId::GBufferMaterial,
             BlackboardResourceId::GBufferDepth,
             BlackboardResourceId::SsaoOcclusion,
             BlackboardResourceId::DepthHazeColor}) {
        invalidate(id, reason);
    }
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

const BlackboardResourceEntry* RenderResourceBlackboard::findProduced(
    BlackboardResourceId id) const noexcept
{
    const BlackboardResourceEntry* entry = find(id);
    return entry != nullptr && producedThisFrame(id) && entry->contentValid
        ? entry : nullptr;
}

bool RenderResourceBlackboard::resolveGBuffer(
    BlackboardGBufferView& view) const noexcept
{
    view = {};
    const BlackboardResourceEntry* albedo =
        findProduced(BlackboardResourceId::GBufferAlbedo);
    const BlackboardResourceEntry* normal =
        findProduced(BlackboardResourceId::GBufferNormal);
    const BlackboardResourceEntry* worldPosition =
        findProduced(BlackboardResourceId::GBufferWorldPosition);
    const BlackboardResourceEntry* material =
        findProduced(BlackboardResourceId::GBufferMaterial);
    const BlackboardResourceEntry* depth =
        findProduced(BlackboardResourceId::GBufferDepth);
    if (albedo == nullptr || normal == nullptr || worldPosition == nullptr
        || material == nullptr || depth == nullptr) {
        return false;
    }

    const bool coherent =
        albedo->framebuffer.idx == normal->framebuffer.idx
        && albedo->framebuffer.idx == worldPosition->framebuffer.idx
        && albedo->framebuffer.idx == material->framebuffer.idx
        && albedo->framebuffer.idx == depth->framebuffer.idx
        && albedo->width == normal->width
        && albedo->width == worldPosition->width
        && albedo->width == material->width
        && albedo->width == depth->width
        && albedo->height == normal->height
        && albedo->height == worldPosition->height
        && albedo->height == material->height
        && albedo->height == depth->height
        && albedo->generation == normal->generation
        && albedo->generation == worldPosition->generation
        && albedo->generation == material->generation
        && albedo->generation == depth->generation;
    if (!coherent || !bgfx::isValid(albedo->framebuffer)
        || !bgfx::isValid(albedo->texture)
        || !bgfx::isValid(normal->texture)
        || !bgfx::isValid(worldPosition->texture)
        || !bgfx::isValid(material->texture)
        || !bgfx::isValid(depth->texture)) {
        return false;
    }

    view.framebuffer = albedo->framebuffer;
    view.albedo = albedo->texture;
    view.normal = normal->texture;
    view.worldPosition = worldPosition->texture;
    view.material = material->texture;
    view.depth = depth->texture;
    view.width = albedo->width;
    view.height = albedo->height;
    view.generation = albedo->generation;
    return true;
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
    case BlackboardResourceId::GBufferAlbedo: return "GBufferAlbedo";
    case BlackboardResourceId::GBufferNormal: return "GBufferNormal";
    case BlackboardResourceId::GBufferWorldPosition: return "GBufferWorldPosition";
    case BlackboardResourceId::GBufferMaterial: return "GBufferMaterial";
    case BlackboardResourceId::GBufferDepth: return "GBufferDepth";
    case BlackboardResourceId::SsaoOcclusion: return "SsaoOcclusion";
    case BlackboardResourceId::DepthHazeColor: return "DepthHazeColor";
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
