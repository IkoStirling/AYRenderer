#include "detail/PresentationStagePlan.h"
#include "detail/RenderViewCatalog.h"

#include <array>

namespace ayt::render::detail {

const char* presentationPlanErrorName(PresentationPlanError error) noexcept
{
    switch(error) {
    case PresentationPlanError::None:return "none";
    case PresentationPlanError::MissingDescriptor:return "missing-descriptor";
    case PresentationPlanError::InvalidOutput:return "invalid-color-output";
    case PresentationPlanError::DuplicateSlot:return "duplicate-stage-slot";
    case PresentationPlanError::DuplicateOutput:return "duplicate-color-output";
    case PresentationPlanError::DuplicateView:return "duplicate-stage-view";
    case PresentationPlanError::InvalidOrder:return "invalid-color-stage-order";
    case PresentationPlanError::InvalidOutputContract:return "incompatible-color-output-contract";
    case PresentationPlanError::InvalidImport:return "invalid-extra-resource-import";
    case PresentationPlanError::ImportOverwritesColor:return "import-overwrites-color-producer";
    case PresentationPlanError::ConflictingImport:return "conflicting-extra-resource-import";
    case PresentationPlanError::OutputAlreadyDeclared:return "color-output-already-declared";
    case PresentationPlanError::ReservedView:return "reserved-stage-view";
    }
    return "unknown-presentation-error";
}

PresentationPlanValidation validatePresentationStagePlan(
    std::span<const PresentationStageFrame> stages,FgResourceId source) noexcept
{
    std::array<bool, 256> slots{};
    std::array<bool, 256> views{};
    std::array<bool, static_cast<size_t>(FgResourceId::Count)> outputs{};
    uint16_t previousOrder = 0;
    bool havePrevious = false;
    // Validate the complete declaration first, including disabled stages. A
    // toggle must never make a structurally invalid description become valid.
    for (size_t index=0;index<stages.size();++index) {
        const auto& stage=stages[index];
        if (!stage.info) return {PresentationPlanError::MissingDescriptor,index};
        const auto& info = *stage.info;
        const auto output = static_cast<size_t>(info.output);
        const auto slot = static_cast<uint8_t>(info.slot);
        if (info.name.empty() || output >= outputs.size() || info.output==source)
            return {PresentationPlanError::InvalidOutput,index,info.output};
        if (outputs[output]) return {PresentationPlanError::DuplicateOutput,index,info.output};
        if (slots[slot]) return {PresentationPlanError::DuplicateSlot,index};
        if (views[info.view]) return {PresentationPlanError::DuplicateView,index};
        const auto* canonical=presentationStageInfo(info.slot);
        if(!canReserveMainRenderView(info.view)
            || (isMainRenderViewReserved(info.view) && (!canonical || canonical->view!=info.view)))
            return {PresentationPlanError::ReservedView,index};
        if (info.write.format!=RenderPassResourceFormat::RGBA8
            || static_cast<size_t>(info.write.resource)>=static_cast<size_t>(RenderPassResourceId::Count)
            || info.write.resource!=contractResourceFor(info.output)
            || (info.history && info.canPresentDirectly)
            || info.write.extent!=RenderPassResourceExtent::Full || info.write.withDepth
            || info.write.lifetime!=(info.history?RenderPassResourceLifetime::PersistentHistory
                                                :RenderPassResourceLifetime::Transient))
            return {PresentationPlanError::InvalidOutputContract,index,info.output};
        if (havePrevious && info.order <= previousOrder)
            return {PresentationPlanError::InvalidOrder,index};
        outputs[output] = true;
        slots[slot] = true;
        views[info.view] = true;
        previousOrder = info.order;
        havePrevious = true;
    }
    std::array<bool,static_cast<size_t>(FgResourceId::Count)> importedIds{};
    std::array<uint16_t,static_cast<size_t>(FgResourceId::Count)> importedHandles{};
    for(size_t index=0;index<stages.size();++index) {
        for(const auto& imported:stages[index].imports) {
            const auto resource=static_cast<size_t>(imported.resource);
            if(resource>=outputs.size())return {PresentationPlanError::InvalidImport,index,imported.resource};
            if(imported.resource==source || outputs[resource])
                return {PresentationPlanError::ImportOverwritesColor,index,imported.resource};
            if(importedIds[resource] && importedHandles[resource]!=imported.framebuffer.idx)
                return {PresentationPlanError::ConflictingImport,index,imported.resource};
            importedIds[resource]=true;importedHandles[resource]=imported.framebuffer.idx;
        }
    }
    return {};
}

bool declarePresentationStages(FrameGraph& graph, std::span<const PresentationStageFrame> stages,
    FgResourceId& source,PresentationPlanValidation* validation,bool allowDirectPresentation)
{
    const auto checked=validatePresentationStagePlan(stages,source);
    if(validation)*validation=checked;
    if(!checked.valid())return false;
    // Check against upstream declarations before mutating any stage resource.
    // Two consumers may borrow the same import; neither may silently replace
    // another producer or reinterpret the shared logical identity's handle.
    for(size_t index=0;index<stages.size();++index) {
        const auto& stage=stages[index];if(!stage.enabled)continue;
        auto fail=[&](PresentationPlanError error,FgResourceId resource) {
            if(validation)*validation={error,index,resource};return false;
        };
        if(graph.resourceDeclared(stage.info->output))return fail(PresentationPlanError::OutputAlreadyDeclared,stage.info->output);
        for(const auto& imported:stage.imports)
            if(!graph.canImportExternal(imported.resource,imported.framebuffer))
                return fail(PresentationPlanError::ConflictingImport,imported.resource);
    }
    const PresentationStageFrame* terminal=nullptr;
    if(allowDirectPresentation)
        for(const auto& stage:stages)if(stage.enabled)terminal=&stage;
    for (const auto& stage : stages) {
        const auto& info = *stage.info;
        graph.setExecutionEligibility(info.slot, stage.enabled);
        if (!stage.enabled) continue;
        for (const auto& imported : stage.imports)
            graph.importExternal(imported.resource, imported.framebuffer);
        if (&stage==terminal && info.canPresentDirectly) {
            graph.importExternal(info.output,BGFX_INVALID_HANDLE);
            graph.setDirectPresentationOutput(info.output, {bgfx::TextureFormat::RGBA8,
                FgTextureScale::Full, true, false, info.pointSampled});
        } else if (info.history) {
            graph.importExternal(info.output, stage.historyTarget);
        } else {
            graph.addResource(info.output, {bgfx::TextureFormat::RGBA8,
                FgTextureScale::Full, true, false, info.pointSampled});
        }
        std::vector<FgResourceId> reads;
        reads.reserve(1+stage.extraReads.size());
        reads.push_back(source);
        reads.insert(reads.end(), stage.extraReads.begin(), stage.extraReads.end());
        graph.addPass({info.name.data(), std::move(reads), {info.output}, true,
            info.history, static_cast<int16_t>(static_cast<uint8_t>(info.slot))});
        source = info.output;
    }
    return true;
}

} // namespace ayt::render::detail
