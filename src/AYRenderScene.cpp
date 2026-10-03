#include "AYRenderer/RenderScene.h"

#include <optional>

namespace ayt::render {

struct RenderScene::OwnedDrawData {
    std::vector<ayt::math::Float4x4> bones;
    std::vector<uint32_t> remap;
    std::optional<DrawPayload2D> payload;
    std::optional<ParticleDrawData> particles;
};

void RenderScene::add(const DrawItem& source)
{
    if (_ownership == DrawDataOwnership::Borrowed
        || (source.boneMatrices == nullptr && source.boneRemap == nullptr
            && source.payload == nullptr && source.particleBatch == nullptr)) {
        _items.push_back(source);
        return;
    }

    auto data = std::make_shared<OwnedDrawData>();
    DrawItem item = source;
    // Motion history consumes the full skeleton, not just the draw palette.
    const uint32_t boneCount = source.skeletonJointCount != 0u
        ? source.skeletonJointCount : source.jointCount;
    if (source.boneMatrices != nullptr && boneCount != 0u) {
        data->bones.assign(source.boneMatrices, source.boneMatrices + boneCount);
        item.boneMatrices = data->bones.data();
    } else {
        item.boneMatrices = nullptr;
    }
    if (source.boneRemap != nullptr && source.jointCount != 0u) {
        data->remap.assign(source.boneRemap, source.boneRemap + source.jointCount);
        item.boneRemap = data->remap.data();
    } else {
        item.boneRemap = nullptr;
    }
    if (source.payload != nullptr) {
        data->payload = *source.payload;
        item.payload = &*data->payload;
    }
    if (source.particleBatch != nullptr) {
        // Copies CPU vertices/indices; GPU stream ownership is deliberately
        // not invented here. hasBorrowedGpuStreams() exposes that boundary.
        data->particles = *source.particleBatch;
        item.particleBatch = &*data->particles;
    }

    // Keep backing alive before publishing pointers. Roll back on allocation
    // failure so add() never leaves a half-published draw in the scene.
    _ownedDrawData.push_back(std::move(data));
    try {
        _items.push_back(item);
    } catch (...) {
        _ownedDrawData.pop_back();
        throw;
    }
}

bool RenderScene::hasBorrowedGpuStreams() const noexcept
{
    for (const auto& item : _items) {
        if (item.particleBatch != nullptr
            && (item.particleBatch->gpuStream != nullptr
                || item.particleBatch->submitGpu != nullptr)) return true;
    }
    return false;
}

} // namespace ayt::render
