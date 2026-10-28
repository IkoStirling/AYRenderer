#include "detail/PresentationStageCatalog.h"
#include "detail/BuiltinShaderCatalog.h"

#include <string>

namespace ayt::render::detail {
namespace {
using Id=RenderPassResourceId;
constexpr auto kColorReads=std::to_array<RenderPassResourceRead>({
    {Id::FinalLdrColor,true},{Id::TaaColor,false},
    {Id::FxaaColor,false},{Id::SmaaColor,false}});

std::span<const RenderPassResourceRead> extraReads(RenderPassSlot slot)
{
    switch(slot) {
#define AY_PRESENTATION_STAGE(Slot, Output, View, Order, Inputs, Point, History, Job, Direct, PreViews) \
    case RenderPassSlot::Slot:return presentationStageExtraReads<RenderPassSlot::Slot>();
#include "detail/PresentationStages.def"
#undef AY_PRESENTATION_STAGE
    default:return {};
    }
}

struct ContractStorage {
    std::array<RenderPassResourceRead,kColorReads.size()+static_cast<size_t>(RenderPassResourceId::Count)> reads{};
    size_t readCount=0;
    RenderPassContract contract;
};

const auto& contracts()
{
    // Built once, independently of GPU initialization. Spans remain valid for
    // the program lifetime, including the unchanged static contract validator.
    static const auto storage=[] {
        std::array<ContractStorage,kPresentationStages.size()> result;
        for(size_t i=0;i<result.size();++i) {
            auto& entry=result[i];const auto& info=kPresentationStages[i];
            const auto additional=extraReads(info.slot);
            if(additional.size()>entry.reads.size()-kColorReads.size())continue;
            size_t count=0;
            for(const auto& read:kColorReads)entry.reads[count++]=read;
            for(const auto& read:additional) {
                if(count>=entry.reads.size())break;
                entry.reads[count++]=read;
            }
            entry.readCount=count;
            // Pointers into the final static storage are assigned below, after
            // returning the array; moving an inline array would invalidate them.
            entry.contract={info.slot,info.name,{}, {&info.write,1},info.history};
        }
        return result;
    }();
    return storage;
}
}

RenderPassContract presentationStageContract(RenderPassSlot slot) noexcept
{
    const auto* info=presentationStageInfo(slot);
    if(!info)return {};
    const auto& entry=contracts()[static_cast<size_t>(info-kPresentationStages.data())];
    auto contract=entry.contract;
    if(contract.name.empty())return {};
    contract.reads={entry.reads.data(),entry.readCount};
    return contract;
}

void appendPrimaryPresentationProgram(std::vector<shader::ShaderBuildJob>& jobs,
    std::string_view name,const char* source)
{
    jobs.push_back(embeddedShader(std::string(name),source?source:""));
}

void appendPresentationStageShaders(std::vector<shader::ShaderBuildJob>& jobs)
{
#define AY_PRESENTATION_STAGE(Slot, Output, View, Order, Inputs, Point, History, Job, Direct, PreViews) \
    appendPresentationStagePrograms<RenderPassSlot::Slot>(jobs);
#include "detail/PresentationStages.def"
#undef AY_PRESENTATION_STAGE
}

} // namespace ayt::render::detail
