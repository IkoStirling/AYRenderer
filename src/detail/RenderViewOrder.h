#pragma once

#include "detail/BGFXAdapter.h"

#include <array>
#include <cstddef>

namespace ayt::render::detail
{

// AYRenderer reserves the 8-bit ViewId range (0..255); UI owns 255.
inline constexpr std::size_t kRenderViewCapacity = 256u;

// Stable view IDs are kept for ABI/debug tooling, while execution follows the
// actual data dependencies. SSAO consumes GBuffer and completes before
// Lighting, which applies AO only to ambient/IBL. Opaque haze then completes
// before transparent composition, and Bloom consumes the resulting HDR color.
inline constexpr std::array<bgfx::ViewId, 31> kRenderViewOrder = {
    1, 18, 19, 20, 21, 22, 23, 24, 25,
    2, 0,
    3, 5, 6, 7, 14,
    8, 13, 9, 253, 254, 10, 11, 12,
    15, 17, 4, 16, 250, 251, 252
};

// bgfx::setViewOrder does not accept a sparse execution list. Its remap table
// is a complete rank -> ViewId permutation, and Frame::sort later inverts that
// permutation. Passing only kRenderViewOrder left high editor views (250+)
// mapped again at their default ranks, so the later duplicate won and those
// views still executed after Present. Fill every unused rank exactly once.
inline constexpr std::array<bgfx::ViewId, kRenderViewCapacity>
makeRenderViewRemap()
{
    std::array<bgfx::ViewId, kRenderViewCapacity> remap{};
    std::array<bool, kRenderViewCapacity> used{};
    std::size_t rank = 0;
    for (const bgfx::ViewId viewId : kRenderViewOrder) {
        remap[rank++] = viewId;
        used[static_cast<std::size_t>(viewId)] = true;
    }
    for (std::size_t viewId = 0; viewId < kRenderViewCapacity; ++viewId) {
        if (!used[viewId]) {
            remap[rank++] = static_cast<bgfx::ViewId>(viewId);
        }
    }
    return remap;
}

inline constexpr auto kRenderViewRemap = makeRenderViewRemap();

inline void configureRenderViewOrder(BGFXAdapter& adapter)
{
    adapter.setViewOrder(0,
                         static_cast<uint16_t>(kRenderViewRemap.size()),
                         kRenderViewRemap.data());
}

} // namespace ayt::render::detail
