#pragma once

#include "AYRenderer/RenderTypes.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ayt::render::detail
{

// Logical resources used to validate the mounted RenderPipeline before any
// GPU object is destroyed or created. These are deliberately broader than
// FgResourceId: the contract also covers pass-owned GBuffer/shadow/history
// targets and the external scene/backbuffer boundaries.
enum class RenderPassResourceId : uint8_t {
    SceneColor = 0,
    SceneDepth,
    ShadowAtlas,
    SkyColor,
    GBufferAlbedo,
    GBufferNormal,
    GBufferWorldPosition,
    GBufferSurface,
    GBufferDepth,
    MotionVectors,
    SSAO,
    HazeColor,
    BloomBright,
    BloomBlurA,
    BloomBlurB,
    FinalLdrColor,
    TaaColor,
    FxaaColor,
    SmaaEdges,
    SmaaBlendWeights,
    SmaaColor,
    ColorGradedColor,
    SelectionMask,
    PresentedColor,
    BloomPyramidQuarter,
    BloomPyramidEighth,
    BloomPyramidSixteenth,
    BloomPyramidUpEighth,
    BloomPyramidUpQuarter,
    AutoExposureHistory,
    AutoExposure,
    Count,
};

enum class RenderPassResourceLifetime : uint8_t {
    External = 0,
    Transient,
    Persistent,
    PersistentHistory,
};

enum class RenderPassResourceFormat : uint8_t {
    Unknown = 0,
    R8,
    RG16F,
    RGBA8,
    RGBA16F,
    D24S8,
};

enum class RenderPassResourceExtent : uint8_t {
    Full = 0,
    Half,
    Atlas,
    Backbuffer,
    Quarter,
    Eighth,
    Sixteenth,
};

struct RenderPassResourceRead {
    RenderPassResourceId resource = RenderPassResourceId::SceneColor;
    bool required = true;
};

struct RenderPassResourceWrite {
    RenderPassResourceId resource = RenderPassResourceId::SceneColor;
    RenderPassResourceLifetime lifetime =
        RenderPassResourceLifetime::Transient;
    RenderPassResourceFormat format = RenderPassResourceFormat::Unknown;
    RenderPassResourceExtent extent = RenderPassResourceExtent::Full;
    bool withDepth = false;
};

struct RenderPassContract {
    RenderPassSlot slot = RenderPassSlot::Shadow;
    std::string_view name;
    std::span<const RenderPassResourceRead> reads;
    std::span<const RenderPassResourceWrite> writes;
    bool sideEffect = false;
};

// Returns nullptr for an out-of-range/unknown slot. Contracts have static
// lifetime and allocate nothing.
const RenderPassContract* renderPassContract(RenderPassSlot slot) noexcept;

enum class RenderPipelineContractErrorCode : uint8_t {
    UnknownSlot = 0,
    DuplicateSlot,
    MissingRequiredResource,
    OptionalProducerAfterConsumer,
};

inline constexpr uint16_t kRenderPassNoIndex = 0xffffu;

struct RenderPipelineContractError {
    RenderPipelineContractErrorCode code =
        RenderPipelineContractErrorCode::UnknownSlot;
    uint16_t passIndex = kRenderPassNoIndex;
    RenderPassSlot slot = RenderPassSlot::Shadow;
    RenderPassResourceId resource = RenderPassResourceId::SceneColor;
};

struct RenderPipelineContractValidation {
    std::vector<RenderPipelineContractError> errors;

    bool valid() const noexcept { return errors.empty(); }
};

// SceneColor/SceneDepth are imported from Renderer::sceneFbo and the
// backbuffer is an external sink. All other required reads must have an
// earlier writer in the supplied dispatch order.
// Optional reads may omit their producer; if mounted, it must run earlier too.
RenderPipelineContractValidation validateRenderPipelineContracts(
    std::span<const RenderPassSlot> slots);

const char* renderPipelineContractErrorName(
    RenderPipelineContractErrorCode code) noexcept;
const char* renderPassResourceName(RenderPassResourceId resource) noexcept;

} // namespace ayt::render::detail
