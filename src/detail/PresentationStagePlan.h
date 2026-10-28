#pragma once

#include "detail/PresentationStageCatalog.h"

#include <span>
#include <vector>

namespace ayt::render::detail {

struct PresentationResourceImport {
    FgResourceId resource;
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
};

// One frame's eligibility/resources. Never cache this as a structural plan:
// imported targets, producer validity and history can change on any submission.
struct PresentationStageFrame {
    const PresentationStageInfo* info = nullptr;
    bool enabled = false;
    std::vector<FgResourceId> extraReads;
    std::vector<PresentationResourceImport> imports;
    bgfx::FrameBufferHandle historyTarget = BGFX_INVALID_HANDLE;
};

enum class PresentationPlanError : uint8_t {
    None, MissingDescriptor, InvalidOutput, DuplicateSlot, DuplicateOutput, DuplicateView,
    InvalidOrder, InvalidOutputContract, InvalidImport, ImportOverwritesColor,
    ConflictingImport, OutputAlreadyDeclared,
    ReservedView,
};

struct PresentationPlanValidation {
    PresentationPlanError error = PresentationPlanError::None;
    size_t stageIndex = 0;
    FgResourceId resource = FgResourceId::Count;
    bool valid() const noexcept { return error==PresentationPlanError::None; }
};

const char* presentationPlanErrorName(PresentationPlanError error) noexcept;
PresentationPlanValidation validatePresentationStagePlan(
    std::span<const PresentationStageFrame> stages, FgResourceId source) noexcept;

// Adds a supplied chain without knowing effect names or potential predecessors.
// Returns false on an invalid/duplicate/out-of-order stage description. The
// caller must not execute a graph whose declaration failed.
bool declarePresentationStages(FrameGraph& graph, std::span<const PresentationStageFrame> stages,
    FgResourceId& source, PresentationPlanValidation* validation=nullptr,
    bool allowDirectPresentation=false);


} // namespace ayt::render::detail
