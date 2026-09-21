#include "detail/RenderPassContract.h"

#include <array>
#include <cstddef>

namespace ayt::render::detail
{
namespace
{

using Id = RenderPassResourceId;
using Life = RenderPassResourceLifetime;
using Format = RenderPassResourceFormat;
using Extent = RenderPassResourceExtent;

constexpr RenderPassResourceRead required(Id id) noexcept
{
    return {id, true};
}

constexpr RenderPassResourceRead optional(Id id) noexcept
{
    return {id, false};
}

constexpr RenderPassResourceWrite output(
    Id id,
    Life lifetime,
    Format format,
    Extent extent = Extent::Full,
    bool withDepth = false) noexcept
{
    return {id, lifetime, format, extent, withDepth};
}

constexpr std::array<RenderPassResourceWrite, 1> kShadowWrites = {{
    // Sampled RGBA8 encoded depth with a private D24S8/D32F attachment.
    output(Id::ShadowAtlas, Life::Persistent, Format::RGBA8,
           Extent::Atlas, true),
}};
constexpr std::array<RenderPassResourceWrite, 1> kSkyboxWrites = {{
    output(Id::SkyColor, Life::Persistent, Format::RGBA8),
}};
constexpr std::array<RenderPassResourceRead, 1> kForwardReads = {{
    optional(Id::ShadowAtlas),
}};
constexpr std::array<RenderPassResourceWrite, 2> kForwardWrites = {{
    output(Id::SceneColor, Life::External, Format::RGBA16F,
           Extent::Full, true),
    output(Id::SceneDepth, Life::External, Format::D24S8),
}};
constexpr std::array<RenderPassResourceRead, 4> kTransparentReads = {{
    required(Id::SceneColor),
    required(Id::SceneDepth),
    optional(Id::ShadowAtlas),
    optional(Id::GBufferDepth),
}};
constexpr std::array<RenderPassResourceWrite, 2> kTransparentWrites = {{
    output(Id::SceneColor, Life::External, Format::RGBA16F,
           Extent::Full, true),
    output(Id::SelectionMask, Life::Persistent, Format::RGBA8),
}};
constexpr std::array<RenderPassResourceWrite, 5> kGBufferWrites = {{
    output(Id::GBufferAlbedo, Life::Persistent, Format::RGBA8,
           Extent::Full, true),
    output(Id::GBufferNormal, Life::Persistent, Format::RGBA8),
    output(Id::GBufferWorldPosition, Life::Persistent, Format::RGBA16F),
    output(Id::GBufferSurface, Life::Persistent, Format::RGBA8),
    output(Id::GBufferDepth, Life::Persistent, Format::D24S8),
}};
constexpr std::array<RenderPassResourceRead, 1> kMotionVectorReads = {{
    required(Id::GBufferDepth),
}};
constexpr std::array<RenderPassResourceWrite, 1> kMotionVectorWrites = {{
    output(Id::MotionVectors, Life::Persistent, Format::RGBA16F),
}};
constexpr std::array<RenderPassResourceRead, 4> kSsaoReads = {{
    required(Id::GBufferNormal),
    required(Id::GBufferWorldPosition),
    required(Id::GBufferSurface),
    required(Id::GBufferDepth),
}};
constexpr std::array<RenderPassResourceWrite, 1> kSsaoWrites = {{
    output(Id::SSAO, Life::Transient, Format::RGBA8),
}};
constexpr std::array<RenderPassResourceRead, 8> kLightingReads = {{
    required(Id::GBufferAlbedo),
    required(Id::GBufferNormal),
    required(Id::GBufferWorldPosition),
    required(Id::GBufferSurface),
    required(Id::GBufferDepth),
    optional(Id::ShadowAtlas),
    optional(Id::SkyColor),
    optional(Id::SSAO),
}};
constexpr std::array<RenderPassResourceWrite, 2> kLightingWrites = {{
    output(Id::SceneColor, Life::External, Format::RGBA16F,
           Extent::Full, true),
    output(Id::SceneDepth, Life::External, Format::D24S8),
}};
constexpr std::array<RenderPassResourceRead, 5> kHazeReads = {{
    required(Id::SceneColor),
    optional(Id::GBufferNormal),
    optional(Id::GBufferWorldPosition),
    optional(Id::GBufferSurface),
    optional(Id::GBufferDepth),
}};
constexpr std::array<RenderPassResourceWrite, 1> kHazeWrites = {{
    output(Id::HazeColor, Life::Transient, Format::RGBA16F),
}};
constexpr std::array<RenderPassResourceRead, 1> kOverlay2DReads = {{
    required(Id::SceneColor),
}};
constexpr std::array<RenderPassResourceWrite, 1> kOverlay2DWrites = {{
    output(Id::SceneColor, Life::External, Format::RGBA16F,
           Extent::Full, true),
}};
constexpr std::array<RenderPassResourceRead, 2> kBloomExtractReads = {{
    required(Id::SceneColor),
    optional(Id::HazeColor),
}};
constexpr std::array<RenderPassResourceWrite, 1> kBloomExtractWrites = {{
    output(Id::BloomBright, Life::Transient, Format::RGBA16F,
           Extent::Half),
}};
constexpr std::array<RenderPassResourceRead, 1> kBloomBlurReads = {{
    required(Id::BloomBright),
}};
constexpr std::array<RenderPassResourceWrite, 2> kBloomBlurWrites = {{
    output(Id::BloomBlurA, Life::Transient, Format::RGBA16F, Extent::Half),
    output(Id::BloomBlurB, Life::Transient, Format::RGBA16F, Extent::Half),
}};
constexpr std::array<RenderPassResourceRead, 3> kPostProcessReads = {{
    required(Id::SceneColor),
    optional(Id::HazeColor),
    optional(Id::BloomBlurB),
}};
constexpr std::array<RenderPassResourceWrite, 1> kPostProcessWrites = {{
    output(Id::FinalLdrColor, Life::Transient, Format::RGBA8),
}};
constexpr std::array<RenderPassResourceRead, 3> kTaaReads = {{
    required(Id::FinalLdrColor),
    required(Id::GBufferWorldPosition),
    optional(Id::MotionVectors),
}};
constexpr std::array<RenderPassResourceWrite, 1> kTaaWrites = {{
    output(Id::TaaColor, Life::PersistentHistory, Format::RGBA8),
}};
constexpr std::array<RenderPassResourceRead, 1> kFxaaReads = {{
    required(Id::FinalLdrColor),
}};
constexpr std::array<RenderPassResourceWrite, 1> kFxaaWrites = {{
    output(Id::FxaaColor, Life::Transient, Format::RGBA8),
}};
constexpr std::array<RenderPassResourceRead, 2> kSmaaReads = {{
    required(Id::FinalLdrColor),
    optional(Id::FxaaColor),
}};
constexpr std::array<RenderPassResourceWrite, 3> kSmaaWrites = {{
    output(Id::SmaaEdges, Life::Transient, Format::RGBA8),
    output(Id::SmaaBlendWeights, Life::Transient, Format::RGBA8),
    output(Id::SmaaColor, Life::Transient, Format::RGBA8),
}};
constexpr std::array<RenderPassResourceRead, 4> kGradingReads = {{
    required(Id::FinalLdrColor),
    optional(Id::TaaColor),
    optional(Id::FxaaColor),
    optional(Id::SmaaColor),
}};
constexpr std::array<RenderPassResourceWrite, 1> kGradingWrites = {{
    output(Id::ColorGradedColor, Life::Transient, Format::RGBA8),
}};
constexpr std::array<RenderPassResourceRead, 5> kPresentReads = {{
    required(Id::FinalLdrColor),
    optional(Id::TaaColor),
    optional(Id::FxaaColor),
    optional(Id::SmaaColor),
    optional(Id::ColorGradedColor),
}};
constexpr std::array<RenderPassResourceWrite, 1> kPresentWrites = {{
    output(Id::PresentedColor, Life::External, Format::RGBA8,
           Extent::Backbuffer),
}};
constexpr std::array<RenderPassResourceRead, 1> kUiReads = {{
    required(Id::PresentedColor),
}};
constexpr std::array<RenderPassResourceRead, 2> kEditorOverlayReads = {{
    required(Id::PresentedColor),
    optional(Id::SelectionMask),
}};
constexpr std::array<RenderPassResourceWrite, 1> kBackbufferWrites = {{
    output(Id::PresentedColor, Life::External, Format::RGBA8,
           Extent::Backbuffer),
}};
constexpr std::array<RenderPassResourceRead, 6> kGBufferDebugReads = {{
    required(Id::PresentedColor),
    required(Id::GBufferAlbedo),
    required(Id::GBufferNormal),
    required(Id::GBufferWorldPosition),
    required(Id::GBufferSurface),
    required(Id::GBufferDepth),
}};

constexpr std::span<const RenderPassResourceRead> noReads{};
constexpr std::span<const RenderPassResourceWrite> noWrites{};

const std::array<RenderPassContract, 21>& contracts() noexcept
{
    static const std::array<RenderPassContract, 21> table = {{
        {RenderPassSlot::Shadow, "Shadow", noReads, kShadowWrites, false},
        {RenderPassSlot::Skybox, "Skybox", noReads, kSkyboxWrites, false},
        {RenderPassSlot::ForwardOpaque, "ForwardOpaque", kForwardReads,
         kForwardWrites, false},
        {RenderPassSlot::Transparent, "Transparent", kTransparentReads,
         kTransparentWrites, false},
        {RenderPassSlot::PostProcess, "PostProcess", kPostProcessReads,
         kPostProcessWrites, false},
        {RenderPassSlot::UI, "UI", kUiReads, kBackbufferWrites, true},
        {RenderPassSlot::GBuffer, "GBuffer", noReads, kGBufferWrites, false},
        {RenderPassSlot::Lighting, "Lighting", kLightingReads,
         kLightingWrites, false},
        {RenderPassSlot::BloomExtract, "BloomExtract", kBloomExtractReads,
         kBloomExtractWrites, false},
        {RenderPassSlot::BloomBlur, "BloomBlur", kBloomBlurReads,
         kBloomBlurWrites, false},
        {RenderPassSlot::DepthHaze, "DepthHaze", kHazeReads,
         kHazeWrites, false},
        {RenderPassSlot::SSAO, "SSAO", kSsaoReads, kSsaoWrites, false},
        {RenderPassSlot::GBufferDebug, "GBufferDebug", kGBufferDebugReads,
         kBackbufferWrites, true},
        {RenderPassSlot::EditorOverlay, "EditorOverlay", kEditorOverlayReads,
         kBackbufferWrites, true},
        {RenderPassSlot::Forward2DOpaque, "Forward2DOpaque", kOverlay2DReads,
         kOverlay2DWrites, false},
        {RenderPassSlot::Present, "Present", kPresentReads, kPresentWrites,
         true},
        {RenderPassSlot::FXAA, "FXAA", kFxaaReads, kFxaaWrites, false},
        {RenderPassSlot::ColorGrading, "ColorGrading", kGradingReads,
         kGradingWrites, false},
        {RenderPassSlot::SMAA, "SMAA", kSmaaReads, kSmaaWrites, false},
        {RenderPassSlot::TAA, "TAA", kTaaReads, kTaaWrites, false},
        {RenderPassSlot::MotionVector, "MotionVector", kMotionVectorReads,
         kMotionVectorWrites, false},
    }};
    return table;
}

} // namespace

const RenderPassContract* renderPassContract(RenderPassSlot slot) noexcept
{
    const auto index = static_cast<std::size_t>(slot);
    const auto& table = contracts();
    if (index >= table.size() || table[index].slot != slot) {
        return nullptr;
    }
    return &table[index];
}

RenderPipelineContractValidation validateRenderPipelineContracts(
    std::span<const RenderPassSlot> slots)
{
    RenderPipelineContractValidation result;
    std::array<bool, 256> seenSlots{};
    std::array<bool, static_cast<std::size_t>(Id::Count)> available{};

    // Renderer owns and clears the forward scene target independently of the
    // mounted geometry pass. This preserves valid post-only custom pipelines.
    available[static_cast<std::size_t>(Id::SceneColor)] = true;
    available[static_cast<std::size_t>(Id::SceneDepth)] = true;

    for (std::size_t passIndex = 0; passIndex < slots.size(); ++passIndex) {
        const RenderPassSlot slot = slots[passIndex];
        const auto rawSlot = static_cast<uint8_t>(slot);
        const RenderPassContract* contract = renderPassContract(slot);
        if (contract == nullptr) {
            result.errors.push_back({
                RenderPipelineContractErrorCode::UnknownSlot,
                static_cast<uint16_t>(passIndex), slot, Id::SceneColor});
            continue;
        }
        if (seenSlots[rawSlot]) {
            result.errors.push_back({
                RenderPipelineContractErrorCode::DuplicateSlot,
                static_cast<uint16_t>(passIndex), slot, Id::SceneColor});
            continue;
        }
        seenSlots[rawSlot] = true;

        for (const RenderPassResourceRead& read : contract->reads) {
            const std::size_t resourceIndex =
                static_cast<std::size_t>(read.resource);
            if (read.required && !available[resourceIndex]) {
                result.errors.push_back({
                    RenderPipelineContractErrorCode::MissingRequiredResource,
                    static_cast<uint16_t>(passIndex), slot, read.resource});
            }
        }
        for (const RenderPassResourceWrite& write : contract->writes) {
            available[static_cast<std::size_t>(write.resource)] = true;
        }
    }

    return result;
}

const char* renderPipelineContractErrorName(
    RenderPipelineContractErrorCode code) noexcept
{
    switch (code) {
    case RenderPipelineContractErrorCode::UnknownSlot:
        return "unknown-slot";
    case RenderPipelineContractErrorCode::DuplicateSlot:
        return "duplicate-slot";
    case RenderPipelineContractErrorCode::MissingRequiredResource:
        return "missing-required-resource";
    }
    return "unknown-error";
}

const char* renderPassResourceName(RenderPassResourceId resource) noexcept
{
    static constexpr std::array<const char*,
        static_cast<std::size_t>(RenderPassResourceId::Count)> names = {{
        "SceneColor", "SceneDepth", "ShadowAtlas", "SkyColor",
        "GBufferAlbedo", "GBufferNormal", "GBufferWorldPosition",
        "GBufferSurface", "GBufferDepth", "MotionVectors", "SSAO",
        "HazeColor", "BloomBright", "BloomBlurA", "BloomBlurB",
        "FinalLdrColor", "TaaColor", "FxaaColor", "SmaaEdges",
        "SmaaBlendWeights", "SmaaColor", "ColorGradedColor",
        "SelectionMask", "PresentedColor",
    }};
    const auto index = static_cast<std::size_t>(resource);
    return index < names.size() ? names[index] : "UnknownResource";
}

} // namespace ayt::render::detail
