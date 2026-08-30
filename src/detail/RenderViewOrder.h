#pragma once

#include "detail/BGFXAdapter.h"

#include <array>

namespace ayt::render::detail
{

// Stable view IDs are kept for ABI/debug tooling, while execution follows the
// actual data dependencies. SSAO consumes GBuffer and completes before
// Lighting, which applies AO only to ambient/IBL. Opaque haze then completes
// before transparent composition, and Bloom consumes the resulting HDR color.
inline constexpr std::array<bgfx::ViewId, 28> kRenderViewOrder = {
    1, 18, 19, 20, 21, 22, 23, 24, 25,
    2, 0,
    3, 4, 5, 6, 7, 14,
    8, 13, 9, 10, 11, 12,
    15, 16, 250, 251, 252
};

inline void configureRenderViewOrder(BGFXAdapter& adapter)
{
    adapter.setViewOrder(0,
                         static_cast<uint16_t>(kRenderViewOrder.size()),
                         kRenderViewOrder.data());
}

} // namespace ayt::render::detail
