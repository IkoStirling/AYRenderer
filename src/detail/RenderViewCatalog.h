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

struct RenderViewDefinition {
    uint16_t view;
    std::string_view owner;
    std::span<const uint8_t> preViews{};
    bool ordered=true;
};

inline constexpr auto kRenderViewDefinitions=std::to_array<RenderViewDefinition>({
#define AY_RENDER_VIEW(View, Owner) {View, Owner},
#define AY_RENDER_VIEW_DEFAULT(View, Owner) {View, Owner, {}, false},
#define AY_RENDER_STAGE_VIEW(Slot, View) {View, #Slot, presentationStageInfo(RenderPassSlot::Slot)->preViews},
#include "detail/RenderViews.def"
#undef AY_RENDER_STAGE_VIEW
#undef AY_RENDER_VIEW_DEFAULT
#undef AY_RENDER_VIEW
});

constexpr size_t orderedRenderViewCount(std::span<const RenderViewDefinition> definitions) noexcept
{
    size_t count=0;
    for(const auto& definition:definitions)if(definition.ordered)count+=1+definition.preViews.size();
    return count;
}

constexpr bool fillOrderedRenderViews(std::span<const RenderViewDefinition> definitions,
    std::span<RenderViewInfo> output) noexcept
{
    if(output.size()!=orderedRenderViewCount(definitions))return false;
    size_t next=0;
    for(const auto& definition:definitions)if(definition.ordered) {
        for(auto view:definition.preViews)output[next++]={view,definition.owner};
        output[next++]={definition.view,definition.owner};
    }
    return true;
}

inline constexpr auto kOrderedRenderViews=[] {
    std::array<RenderViewInfo,orderedRenderViewCount(kRenderViewDefinitions)> result{};
    fillOrderedRenderViews(kRenderViewDefinitions,result);
    return result;
}();

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

constexpr auto makeRenderViewReservations(std::span<const RenderViewInfo> views) noexcept
{
    std::array<bool,kRenderViewCapacity> reserved{};
    for(const auto& info:views)if(info.view<reserved.size())reserved[info.view]=true;
    return reserved;
}
inline constexpr auto kMainRenderViewReservations=makeRenderViewReservations(kOrderedRenderViews);
constexpr bool isMainRenderViewReserved(uint16_t view) noexcept
{
    return view<kMainRenderViewReservations.size() && kMainRenderViewReservations[view];
}

constexpr auto makeRenderViewOwners(std::span<const RenderViewDefinition> definitions) noexcept
{
    std::array<std::string_view,kRenderViewCapacity> owners{};
    for(const auto& definition:definitions) {
        if(definition.view<owners.size())owners[definition.view]=definition.owner;
        for(auto view:definition.preViews)owners[view]=definition.owner;
    }
    return owners;
}
inline constexpr auto kRenderViewOwners=makeRenderViewOwners(kRenderViewDefinitions);

constexpr std::string_view renderViewOwnerName(uint16_t view) noexcept
{
    return view<kRenderViewOwners.size()?kRenderViewOwners[view]:std::string_view{};
}

} // namespace ayt::render::detail
