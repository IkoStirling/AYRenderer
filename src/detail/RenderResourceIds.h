#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ayt::render::detail {

inline constexpr auto kFrameGraphResourceNumbers=std::to_array<uint16_t>({
#define AY_RENDER_RESOURCE_SHARED(Fg, FgValue, Contract, ContractValue) FgValue,
#define AY_RENDER_RESOURCE_FRAME(Fg, Value) Value,
#define AY_RENDER_RESOURCE_CONTRACT(Contract, Value)
#include "detail/RenderResourceIds.def"
#undef AY_RENDER_RESOURCE_CONTRACT
#undef AY_RENDER_RESOURCE_FRAME
#undef AY_RENDER_RESOURCE_SHARED
});
inline constexpr auto kPassContractResourceNumbers=std::to_array<uint16_t>({
#define AY_RENDER_RESOURCE_SHARED(Fg, FgValue, Contract, ContractValue) ContractValue,
#define AY_RENDER_RESOURCE_FRAME(Fg, Value)
#define AY_RENDER_RESOURCE_CONTRACT(Contract, Value) Value,
#include "detail/RenderResourceIds.def"
#undef AY_RENDER_RESOURCE_CONTRACT
#undef AY_RENDER_RESOURCE_FRAME
#undef AY_RENDER_RESOURCE_SHARED
});

constexpr bool denseResourceNumbers(std::span<const uint16_t> numbers) noexcept
{
    if(numbers.empty() || numbers.size()>=256)return false;
    std::array<bool,256> used{};
    for(const auto value:numbers) {
        if(value>=numbers.size() || used[value])return false;
        used[value]=true;
    }
    return true;
}
static_assert(denseResourceNumbers(kFrameGraphResourceNumbers));
static_assert(denseResourceNumbers(kPassContractResourceNumbers));

enum class FgResourceId : uint8_t {
#define AY_RENDER_RESOURCE_SHARED(Fg, FgValue, Contract, ContractValue) Fg=FgValue,
#define AY_RENDER_RESOURCE_FRAME(Fg, Value) Fg=Value,
#define AY_RENDER_RESOURCE_CONTRACT(Contract, Value)
#include "detail/RenderResourceIds.def"
#undef AY_RENDER_RESOURCE_CONTRACT
#undef AY_RENDER_RESOURCE_FRAME
#undef AY_RENDER_RESOURCE_SHARED
    HazeHalf=HazeColor, // Source-compatible alias of the full-resolution HDR color.
    Count=kFrameGraphResourceNumbers.size(),
};

enum class RenderPassResourceId : uint8_t {
#define AY_RENDER_RESOURCE_SHARED(Fg, FgValue, Contract, ContractValue) Contract=ContractValue,
#define AY_RENDER_RESOURCE_FRAME(Fg, Value)
#define AY_RENDER_RESOURCE_CONTRACT(Contract, Value) Contract=Value,
#include "detail/RenderResourceIds.def"
#undef AY_RENDER_RESOURCE_CONTRACT
#undef AY_RENDER_RESOURCE_FRAME
#undef AY_RENDER_RESOURCE_SHARED
    Count=kPassContractResourceNumbers.size(),
};

inline constexpr auto kFrameGraphResourceNames=[] {
    std::array<const char*,kFrameGraphResourceNumbers.size()> names{};
#define AY_RENDER_RESOURCE_SHARED(Fg, FgValue, Contract, ContractValue) names[FgValue]=#Fg;
#define AY_RENDER_RESOURCE_FRAME(Fg, Value) names[Value]=#Fg;
#define AY_RENDER_RESOURCE_CONTRACT(Contract, Value)
#include "detail/RenderResourceIds.def"
#undef AY_RENDER_RESOURCE_CONTRACT
#undef AY_RENDER_RESOURCE_FRAME
#undef AY_RENDER_RESOURCE_SHARED
    return names;
}();
inline constexpr auto kPassContractResourceNames=[] {
    std::array<const char*,kPassContractResourceNumbers.size()> names{};
#define AY_RENDER_RESOURCE_SHARED(Fg, FgValue, Contract, ContractValue) names[ContractValue]=#Contract;
#define AY_RENDER_RESOURCE_FRAME(Fg, Value)
#define AY_RENDER_RESOURCE_CONTRACT(Contract, Value) names[Value]=#Contract;
#include "detail/RenderResourceIds.def"
#undef AY_RENDER_RESOURCE_CONTRACT
#undef AY_RENDER_RESOURCE_FRAME
#undef AY_RENDER_RESOURCE_SHARED
    return names;
}();

inline constexpr auto kFrameToContractResource=[] {
    std::array<RenderPassResourceId,kFrameGraphResourceNumbers.size()> ids{};
    ids.fill(RenderPassResourceId::Count);
#define AY_RENDER_RESOURCE_SHARED(Fg, FgValue, Contract, ContractValue) ids[FgValue]=RenderPassResourceId::Contract;
#define AY_RENDER_RESOURCE_FRAME(Fg, Value)
#define AY_RENDER_RESOURCE_CONTRACT(Contract, Value)
#include "detail/RenderResourceIds.def"
#undef AY_RENDER_RESOURCE_CONTRACT
#undef AY_RENDER_RESOURCE_FRAME
#undef AY_RENDER_RESOURCE_SHARED
    return ids;
}();
inline constexpr auto kContractToFrameResource=[] {
    std::array<FgResourceId,kPassContractResourceNumbers.size()> ids{};
    ids.fill(FgResourceId::Count);
#define AY_RENDER_RESOURCE_SHARED(Fg, FgValue, Contract, ContractValue) ids[ContractValue]=FgResourceId::Fg;
#define AY_RENDER_RESOURCE_FRAME(Fg, Value)
#define AY_RENDER_RESOURCE_CONTRACT(Contract, Value)
#include "detail/RenderResourceIds.def"
#undef AY_RENDER_RESOURCE_CONTRACT
#undef AY_RENDER_RESOURCE_FRAME
#undef AY_RENDER_RESOURCE_SHARED
    return ids;
}();

constexpr const char* frameGraphResourceName(FgResourceId id) noexcept
{
    const auto index=static_cast<std::size_t>(id);
    return index<kFrameGraphResourceNames.size()?kFrameGraphResourceNames[index]:"UnknownResource";
}
constexpr const char* contractResourceName(RenderPassResourceId id) noexcept
{
    const auto index=static_cast<std::size_t>(id);
    return index<kPassContractResourceNames.size()?kPassContractResourceNames[index]:"UnknownResource";
}
constexpr RenderPassResourceId contractResourceFor(FgResourceId id) noexcept
{
    const auto index=static_cast<std::size_t>(id);
    return index<kFrameToContractResource.size()?kFrameToContractResource[index]:RenderPassResourceId::Count;
}
constexpr FgResourceId frameGraphResourceFor(RenderPassResourceId id) noexcept
{
    const auto index=static_cast<std::size_t>(id);
    return index<kContractToFrameResource.size()?kContractToFrameResource[index]:FgResourceId::Count;
}
}
