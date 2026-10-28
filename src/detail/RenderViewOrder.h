#pragma once

#include "detail/BGFXAdapter.h"
#include "detail/RenderViewCatalog.h"

#include <array>
#include <cstddef>

namespace ayt::render::detail
{

// AYRenderer reserves the 8-bit ViewId range (0..255); UI owns 255.

// Stable view IDs are kept for ABI/debug tooling, while execution follows the
// actual data dependencies. SSAO consumes GBuffer and completes before
// Lighting, which applies AO only to ambient/IBL. Opaque haze then completes
// before transparent composition, and Bloom consumes the resulting HDR color.
// GPU particle compute runs before both transparent 3D and overlay 2D draws.
inline constexpr auto kRenderViewOrder = [] {
    std::array<bgfx::ViewId,kOrderedRenderViews.size()> order{};
    for(std::size_t i=0;i<order.size();++i)order[i]=kOrderedRenderViews[i].view;
    return order;
}();

static_assert([] {
    if(kRenderViewOrder.size()>kRenderViewCapacity)return false;
    std::array<bool,kRenderViewCapacity> used{};
    for(auto view:kRenderViewOrder) {
        if(view>=used.size() || used[view])return false;
        used[view]=true;
    }
    return true;
}(), "Built-in and presentation GPU Views must be unique and fit the remap");

// bgfx::setViewOrder does not accept a sparse execution list. Its remap table
// is a complete rank -> ViewId permutation, and Frame::sort later inverts that
// permutation. Passing only kRenderViewOrder left high editor views (250+)
// mapped again at their default ranks, so the later duplicate won and those
// views still executed after Present. Fill every unused rank exactly once.
inline constexpr bool fillRenderViewRemap(std::span<const RenderViewInfo> views,
    std::array<bgfx::ViewId,kRenderViewCapacity>& remap) noexcept
{
    if(views.size()>remap.size() || !validMainRenderViews(views))return false;
    std::array<bool, kRenderViewCapacity> used{};
    std::size_t rank = 0;
    for (const auto& view : views) {
        const auto viewId=static_cast<bgfx::ViewId>(view.view);
        remap[rank++] = viewId;
        used[static_cast<std::size_t>(viewId)] = true;
    }
    for (std::size_t viewId = 0; viewId < kRenderViewCapacity; ++viewId) {
        if (!used[viewId]) {
            remap[rank++] = static_cast<bgfx::ViewId>(viewId);
        }
    }
    return true;
}

inline constexpr std::array<bgfx::ViewId,kRenderViewCapacity> makeRenderViewRemap()
{
    std::array<bgfx::ViewId,kRenderViewCapacity> remap{};
    fillRenderViewRemap(kOrderedRenderViews,remap);
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
