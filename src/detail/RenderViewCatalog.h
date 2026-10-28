#pragma once

#include "detail/PresentationStageCatalog.h"
#include "detail/RenderViewRanges.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace ayt::render::detail {

struct RenderViewInfo {
    uint16_t view;
    std::string_view owner;
};

inline constexpr auto kOrderedRenderViews=std::to_array<RenderViewInfo>({
#define AY_RENDER_VIEW(View, Owner) {View, Owner},
#define AY_RENDER_VIEW_DEFAULT(View, Owner)
#include "detail/RenderViews.def"
#undef AY_RENDER_VIEW_DEFAULT
#undef AY_RENDER_VIEW
});

constexpr bool validMainRenderViews(std::span<const RenderViewInfo> views) noexcept
{
    std::array<bool,kRenderViewCapacity> used{};
    for(const auto& info:views) {
        if(!canReserveMainRenderView(info.view) || used[info.view])return false;
        used[info.view]=true;
    }
    return true;
}
static_assert(validMainRenderViews(kOrderedRenderViews),
    "Main GPU Views must be unique and outside camera/readback/default UI reservations");

inline constexpr auto kMainRenderViewReservations=[] {
    std::array<bool,kRenderViewCapacity> reserved{};
    for(const auto& info:kOrderedRenderViews)reserved[info.view]=true;
    return reserved;
}();
constexpr bool isMainRenderViewReserved(uint16_t view) noexcept
{
    return view<kMainRenderViewReservations.size() && kMainRenderViewReservations[view];
}

inline constexpr auto kRenderViewOwners=[] {
    std::array<std::string_view,kRenderViewCapacity> owners{};
#define AY_RENDER_VIEW(View, Owner) owners[View]=Owner;
#define AY_RENDER_VIEW_DEFAULT(View, Owner) owners[View]=Owner;
#include "detail/RenderViews.def"
#undef AY_RENDER_VIEW_DEFAULT
#undef AY_RENDER_VIEW
    return owners;
}();

constexpr std::string_view renderViewOwnerName(uint16_t view) noexcept
{
    return view<kRenderViewOwners.size()?kRenderViewOwners[view]:std::string_view{};
}

} // namespace ayt::render::detail
