#pragma once

#include "detail/FgResource.h"
#include "detail/RenderPassContract.h"
#include "detail/RenderViewRanges.h"

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

namespace ayt::render::detail {

enum class PresentationStageInputs : uint8_t { Color, Geometry, History };
class RenderPass;
class PresentationPass;

template<RenderPassSlot Slot> std::unique_ptr<RenderPass> makePresentationStage();
template<RenderPassSlot Slot> const char* presentationStageShaderSource();
template<RenderPassSlot Slot> std::span<const RenderPassResourceRead> presentationStageExtraReads();

// Declarations make a missing implementation a link failure. No static
// registrars, global mutable map or per-effect includes in the common factory.
#define AY_PRESENTATION_STAGE(Slot, Output, View, Order, Inputs, Point, History, Job, Direct) \
    template<> std::unique_ptr<RenderPass> makePresentationStage<RenderPassSlot::Slot>(); \
    template<> const char* presentationStageShaderSource<RenderPassSlot::Slot>(); \
    template<> std::span<const RenderPassResourceRead> presentationStageExtraReads<RenderPassSlot::Slot>();
#include "detail/PresentationStages.def"
#undef AY_PRESENTATION_STAGE

// Define once inside ayt::render::detail in the concrete stage's .cpp. Extra
// contract reads must have static lifetime. Ordinary color stages pass an empty
// span; geometry/history stages declare only their own additional resources.
#define AY_DEFINE_PRESENTATION_STAGE(Slot, Type, Source, Reads) \
    template<> std::unique_ptr<RenderPass> makePresentationStage<RenderPassSlot::Slot>() \
    { static_assert(std::is_base_of_v<PresentationPass,Type>); return std::make_unique<Type>(); } \
    template<> const char* presentationStageShaderSource<RenderPassSlot::Slot>() \
    { return Source; } \
    template<> std::span<const RenderPassResourceRead> presentationStageExtraReads<RenderPassSlot::Slot>() \
    { static_assert((Reads).size()<=static_cast<size_t>(RenderPassResourceId::Count)); \
      static_assert([] { for(const auto& read:Reads) \
        if(static_cast<size_t>(read.resource)>=static_cast<size_t>(RenderPassResourceId::Count))return false; \
        return true; }()); return Reads; }

// Immutable structural data only. Programs, history and settings keep their
// existing owners. A descriptor does not imply that its stage runs this frame.
struct PresentationStageInfo {
    RenderPassSlot slot;
    std::string_view name;
    FgResourceId output;
    RenderPassResourceWrite write;
    uint8_t view;
    uint16_t order;
    PresentationStageInputs inputs;
    bool pointSampled;
    bool history;
    std::string_view shaderJob;
    std::string_view outputName;
    // Opt in only after validating backbuffer transfer/viewport equivalence.
    // History outputs must remain sampleable and cannot use this path.
    bool canPresentDirectly = false;
};

inline constexpr auto kPresentationStages = std::to_array<PresentationStageInfo>({
#define AY_PRESENTATION_STAGE(Slot, Output, View, Order, Inputs, Point, History, Job, Direct) \
    {RenderPassSlot::Slot, #Slot, FgResourceId::Output, \
     {RenderPassResourceId::Output, History ? RenderPassResourceLifetime::PersistentHistory \
                                          : RenderPassResourceLifetime::Transient, \
      RenderPassResourceFormat::RGBA8}, View, Order, PresentationStageInputs::Inputs, Point, History, Job, #Output, Direct},
#include "detail/PresentationStages.def"
#undef AY_PRESENTATION_STAGE
});

inline constexpr auto kPresentationStageBySlot=[] {
    std::array<const PresentationStageInfo*,256> stages{};
    for(const auto& info:kPresentationStages)stages[static_cast<uint8_t>(info.slot)]=&info;
    return stages;
}();

constexpr const PresentationStageInfo* presentationStageInfo(RenderPassSlot slot) noexcept
{
    return kPresentationStageBySlot[static_cast<uint8_t>(slot)];
}

constexpr uint8_t presentationStageViewId(RenderPassSlot slot) noexcept
{
    const auto* stage = presentationStageInfo(slot);
    return stage ? stage->view : UINT8_MAX;
}

constexpr std::string_view presentationStageNameForView(uint16_t view) noexcept
{
    for (const auto& stage : kPresentationStages)
        if (stage.view == view) return stage.name;
    return {};
}

constexpr bool validPresentationStageCatalog(std::span<const PresentationStageInfo> stages) noexcept
{
    for (size_t i=0; i<stages.size(); ++i) {
        const auto& stage=stages[i];
        if (stage.name.empty() || stage.shaderJob.empty() || stage.outputName.empty() || !canReserveMainRenderView(stage.view)
            || static_cast<size_t>(stage.output)>=static_cast<size_t>(FgResourceId::Count)
            || static_cast<size_t>(stage.write.resource)>=static_cast<size_t>(RenderPassResourceId::Count)
            || stage.write.resource!=contractResourceFor(stage.output)
            || stage.write.format!=RenderPassResourceFormat::RGBA8
            || stage.write.extent!=RenderPassResourceExtent::Full || stage.write.withDepth
            || (stage.history && stage.canPresentDirectly)
            || stage.write.lifetime!=(stage.history?RenderPassResourceLifetime::PersistentHistory
                                                 :RenderPassResourceLifetime::Transient)
            || (i && stage.order<=stages[i-1].order)) return false;
        for (size_t j=0; j<i; ++j) {
            const auto& previous=stages[j];
            if (stage.slot==previous.slot || stage.output==previous.output
                || stage.write.resource==previous.write.resource || stage.view==previous.view
                || stage.name==previous.name || stage.shaderJob==previous.shaderJob) return false;
        }
    }
    return true;
}

static_assert(validPresentationStageCatalog(kPresentationStages),
    "Presentation stages require unique identities/views/outputs and compatible ordered color contracts");

// The common color input and AA prerequisites are shared. Only genuinely extra
// resources belong in per-kind reads; predecessors are validated by chain order.
RenderPassContract presentationStageContract(RenderPassSlot slot) noexcept;

} // namespace ayt::render::detail
