#pragma once

#include "AYRenderer/DeferredDecal.h"
#include "detail/FrameContext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ayt::render::detail {

// Test the actual projector cube, without a perspective divide. Only reject
// when all eight corners are strictly outside the same clip half-space.
// Unknown arithmetic/transform data stays visible; validation is still the
// responsibility of the complete decal transaction, including rejected items.
inline bool deferredDecalOutsideFrustum(
    const DeferredDecal& decal, const FrameContext& frame) noexcept
{
    const auto& world = decal.localToWorld;
    if (!frame.frustumCulling
        || !std::isfinite(frame.cullingMarginX)
        || !std::isfinite(frame.cullingMarginY)
        || world(3, 0) != 0.0f || world(3, 1) != 0.0f
        || world(3, 2) != 0.0f || world(3, 3) != 1.0f)
        return false;

    // GBuffer world positions are binary16. A visible receiver can round into
    // a projector just beyond a clip plane. Enlarge every world-space plane
    // by a conservative full half-precision ULP, including subnormals.
    float worldError[3]{};
    for (int axis = 0; axis < 3; ++axis) {
        float magnitude = std::abs(world(axis, 3));
        for (int column = 0; column < 3; ++column)
            magnitude += std::abs(world(axis, column));
        worldError[axis] = magnitude / 1024.0f + 0x1p-24f;
        if (!std::isfinite(worldError[axis])) return false;
    }
    const float marginX = std::max(0.0f, frame.cullingMarginX);
    const float marginY = std::max(0.0f, frame.cullingMarginY);
    const auto& vp = frame.cullingViewProjection;
    std::array<float, 6> roundingPad{};
    for (int axis = 0; axis < 3; ++axis) {
        const float x = vp(0, axis), y = vp(1, axis);
        const float z = vp(2, axis), w = vp(3, axis);
        const float weights[6] = {
            std::abs(x+w)+marginX*std::abs(w), std::abs(w-x)+marginX*std::abs(w),
            std::abs(y+w)+marginY*std::abs(w), std::abs(w-y)+marginY*std::abs(w),
            std::abs(frame.cullingHomogeneousDepth ? z+w : z), std::abs(w-z)};
        for (unsigned plane = 0; plane < roundingPad.size(); ++plane)
            roundingPad[plane] += weights[plane] * worldError[axis];
    }
    for (float pad : roundingPad) if (!std::isfinite(pad)) return false;

    const auto matrix = vp * world;
    std::array<bool, 6> outside{true, true, true, true, true, true};
    for (unsigned corner = 0; corner < 8; ++corner) {
        const float local[4] = {
            corner & 1 ? 1.0f : -1.0f,
            corner & 2 ? 1.0f : -1.0f,
            corner & 4 ? 1.0f : -1.0f, 1.0f};
        float clip[4]{};
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col)
                clip[row] += matrix(row, col) * local[col];
            if (!std::isfinite(clip[row])) return false;
        }
        const float padX = marginX * std::abs(clip[3]);
        const float padY = marginY * std::abs(clip[3]);
        const float planes[6] = {
            clip[0] + clip[3] + padX, clip[3] - clip[0] + padX,
            clip[1] + clip[3] + padY, clip[3] - clip[1] + padY,
            frame.cullingHomogeneousDepth ? clip[2] + clip[3] : clip[2],
            clip[3] - clip[2]};
        const float tolerance = 1.0e-5f * std::max({1.0f,
            std::abs(clip[0]), std::abs(clip[1]), std::abs(clip[2]), std::abs(clip[3])});
        for (unsigned plane = 0; plane < outside.size(); ++plane) {
            if (!std::isfinite(planes[plane])) return false;
            outside[plane] = outside[plane]
                && planes[plane] + roundingPad[plane] < -tolerance;
        }
    }
    for (bool rejected : outside) if (rejected) return true;
    return false;
}

struct DeferredDecalVisibility {
    std::array<size_t, kMaxDeferredDecals> indices{};
    size_t count = 0;
};

// Shared by graph planning and standalone pass execution. Source indices stay
// in author order; ping-pong parity and view IDs use the compacted draw index.
inline DeferredDecalVisibility selectDeferredDecals(
    const std::vector<DeferredDecal>& decals, const FrameContext& frame) noexcept
{
    DeferredDecalVisibility result;
    if (decals.size() > kMaxDeferredDecals) return result;
    for (size_t index = 0; index < decals.size(); ++index) {
        if (!deferredDecalOutsideFrustum(decals[index], frame))
            result.indices[result.count++] = index;
    }
    return result;
}

} // namespace ayt::render::detail
