#pragma once

#include "AYRenderer/RenderTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ayt::render::detail
{

// Single source of truth for the deferred attachment and packed-channel ABI.
// Changing an attachment index or format requires matching shader cache/build
// stamp bumps and consumer updates.
struct GBufferLayout final {
    static constexpr uint8_t kAlbedoMetallicAttachment     = 0;
    static constexpr uint8_t kNormalRoughnessAttachment    = 1;
    static constexpr uint8_t kWorldPositionPackedAttachment = 2;
    static constexpr uint8_t kEmissiveCoverageAttachment   = 3;
    static constexpr uint8_t kColorAttachmentCount         = 4;
    static constexpr uint8_t kDepthAttachment              = 4;

    // RT2 is RGBA16F, so AO can stay continuous while model IDs occupy disjoint
    // two-unit intervals: packed = model*2 + AO. This adds model data without
    // increasing MRT bandwidth or sharing the independent RT3.a coverage bit.
    static constexpr float kMaterialModelStride = 2.0f;

    static float packAoAndMaterialModel(float ao,
                                        MaterialModel model) noexcept
    {
        const float clampedAo = std::isfinite(ao)
            ? std::clamp(ao, 0.0f, 1.0f)
            : 1.0f;
        const uint8_t rawModel = std::min<uint8_t>(
            static_cast<uint8_t>(model),
            static_cast<uint8_t>(MaterialModel::Count) - 1u);
        return static_cast<float>(rawModel) * kMaterialModelStride + clampedAo;
    }

    static MaterialModel unpackMaterialModel(float packed) noexcept
    {
        const float nonNegative = std::isfinite(packed)
            ? std::max(0.0f, packed)
            : 0.0f;
        const float maxModel = static_cast<float>(
            static_cast<uint8_t>(MaterialModel::Count) - 1u);
        const uint8_t rawModel = static_cast<uint8_t>(std::clamp(
            std::floor(nonNegative / kMaterialModelStride + 1.0e-4f),
            0.0f, maxModel));
        return static_cast<MaterialModel>(rawModel);
    }

    static float unpackAo(float packed) noexcept
    {
        const float finitePacked = std::isfinite(packed) ? packed : 0.0f;
        const MaterialModel model = unpackMaterialModel(finitePacked);
        return std::clamp(
            finitePacked - static_cast<float>(static_cast<uint8_t>(model))
                * kMaterialModelStride,
            0.0f, 1.0f);
    }
};

} // namespace ayt::render::detail
