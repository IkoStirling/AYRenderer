#include "AYTest.h"

#include "AYRenderer.h"
#include "AYRenderer/RenderTypes.h"
#include "detail/RenderPassContract.h"
#include "detail/RenderPass.h"
#include "detail/RenderPipelineFactory.h"
#include "detail/RenderPipelinePolicy.h"

#include <algorithm>
#include <array>

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::Renderer;
using ayt::render::detail::RenderPassResourceId;
using ayt::render::detail::RenderPassResourceLifetime;
using ayt::render::detail::RenderPassResourceExtent;
using ayt::render::detail::RenderPipelineContractErrorCode;
using ayt::render::detail::renderPassContract;
using ayt::render::detail::validateRenderPipelineContracts;

namespace {

const ayt::render::detail::RenderPassResourceWrite* findWrite(
    const ayt::render::detail::RenderPassContract& contract,
    RenderPassResourceId id)
{
    const auto it = std::find_if(
        contract.writes.begin(), contract.writes.end(),
        [id](const auto& write) { return write.resource == id; });
    return it != contract.writes.end() ? &*it : nullptr;
}

} // namespace

TEST_SUITE(AYRenderer_RenderPassContracts_R6)

TEST_CASE(pass_factory_accepts_exactly_the_static_contract_inventory)
{
    // Compare independent assembly boundaries, including every unknown byte
    // value; adding a contract without a factory must not silently omit a pass.
    for (uint16_t raw = 0; raw < 256; ++raw) {
        const auto slot = static_cast<RenderPassSlot>(raw);
        const auto* contract = renderPassContract(slot);
        auto pass = ayt::render::detail::makeRenderPass(slot);
        CHECK(bool(pass) == bool(contract));
        if (!pass || !contract) continue;
        CHECK(pass->name() == contract->name);
        auto second = ayt::render::detail::makeRenderPass(slot);
        CHECK(second != nullptr);
        CHECK(second.get() != pass.get());
    }
}

TEST_CASE(legacy_normalization_is_pure_idempotent_and_preserves_explicit_errors)
{
    const RenderPipelineDesc legacy{{RenderPassSlot::PostProcess,
        RenderPassSlot::ColorGrading, RenderPassSlot::TemporalFeedback,
        RenderPassSlot::AnalogDisplay, RenderPassSlot::UI}, ayt::render::RenderPath::Deferred};
    const auto original = legacy.passes;
    const auto resolved = ayt::render::detail::resolveRenderPipelineDesc(legacy);
    CHECK(legacy.passes == original);
    CHECK(resolved.path == legacy.path);
    CHECK(validateRenderPipelineContracts(resolved.passes).valid());
    CHECK(resolved.passes.size() == legacy.passes.size()+1);
    if (resolved.passes.size() < 2) return;
    CHECK(resolved.passes[resolved.passes.size()-2] == RenderPassSlot::Present);
    CHECK(resolved.passes.back() == RenderPassSlot::UI);
    CHECK(ayt::render::detail::resolveRenderPipelineDesc(resolved).passes == resolved.passes);

    const RenderPipelineDesc misordered{{RenderPassSlot::PostProcess,
        RenderPassSlot::Present, RenderPassSlot::FXAA}};
    const auto stillMisordered = ayt::render::detail::resolveRenderPipelineDesc(misordered);
    CHECK(stillMisordered.passes == misordered.passes);
    CHECK_FALSE(validateRenderPipelineContracts(stillMisordered.passes).valid());
}

TEST_CASE(every_public_slot_has_one_static_contract)
{
    for (uint8_t raw = 0; raw <= static_cast<uint8_t>(
             RenderPassSlot::Clouds); ++raw) {
        const auto slot = static_cast<RenderPassSlot>(raw);
        const auto* contract = renderPassContract(slot);
        CHECK(contract != nullptr);
        CHECK(contract->slot == slot);
        CHECK(!contract->name.empty());
    }
}

TEST_CASE(canonical_pipeline_variants_satisfy_static_dependencies)
{
    for (const RenderPipelineDesc desc : {
             RenderPipelineDesc::makeDefault(),
             RenderPipelineDesc::makeDeferred(),
             RenderPipelineDesc::makeEditorForward(),
             RenderPipelineDesc::makeEditorDeferred()}) {
        CHECK(validateRenderPipelineContracts(desc.passes).valid());
    }
}

TEST_CASE(output_contracts_pin_transient_and_history_lifetimes)
{
    const auto* bloom = renderPassContract(RenderPassSlot::BloomExtract);
    const auto* taa = renderPassContract(RenderPassSlot::TAA);
    const auto* present = renderPassContract(RenderPassSlot::Present);
    CHECK(bloom != nullptr);
    CHECK(taa != nullptr);
    CHECK(present != nullptr);

    const auto* bloomBright = findWrite(*bloom, RenderPassResourceId::BloomBright);
    const auto* taaColor = findWrite(*taa, RenderPassResourceId::TaaColor);
    const auto* presented = findWrite(*present,
                                      RenderPassResourceId::PresentedColor);
    CHECK(bloomBright != nullptr);
    CHECK(bloomBright->lifetime == RenderPassResourceLifetime::Transient);
    CHECK(bloomBright->extent == RenderPassResourceExtent::Half);
    CHECK(taaColor != nullptr);
    CHECK(taaColor->lifetime ==
          RenderPassResourceLifetime::PersistentHistory);
    CHECK(taaColor->format == ayt::render::detail::RenderPassResourceFormat::RGBA16F);
    CHECK(taa->sideEffect); // Optional late diagnostic output.
    CHECK(presented != nullptr);
    CHECK(presented->lifetime == RenderPassResourceLifetime::External);
    CHECK(present->sideEffect);
}

TEST_CASE(post_only_custom_pipeline_uses_imported_scene_boundary)
{
    constexpr std::array slots = {
        RenderPassSlot::PostProcess,
        RenderPassSlot::FXAA,
        RenderPassSlot::ColorGrading,
        RenderPassSlot::Present,
    };
    CHECK(validateRenderPipelineContracts(slots).valid());
}

TEST_CASE(missing_and_misordered_producers_are_reported_before_dispatch)
{
    constexpr std::array bloomWithoutExtract = {
        RenderPassSlot::BloomBlur,
        RenderPassSlot::PostProcess,
        RenderPassSlot::Present,
    };
    const auto bloomResult =
        validateRenderPipelineContracts(bloomWithoutExtract);
    CHECK(!bloomResult.valid());
    CHECK(bloomResult.errors.front().code ==
          RenderPipelineContractErrorCode::MissingRequiredResource);
    CHECK(bloomResult.errors.front().passIndex == 0u);
    CHECK(bloomResult.errors.front().resource ==
          RenderPassResourceId::BloomBright);

    constexpr std::array lightingBeforeGbuffer = {
        RenderPassSlot::Lighting,
        RenderPassSlot::GBuffer,
    };
    const auto lightingResult =
        validateRenderPipelineContracts(lightingBeforeGbuffer);
    CHECK(!lightingResult.valid());
    CHECK(lightingResult.errors.front().passIndex == 0u);
    CHECK(lightingResult.errors.front().resource ==
          RenderPassResourceId::GBufferAlbedo);
}

TEST_CASE(duplicate_and_unknown_slots_are_rejected)
{
    constexpr std::array duplicate = {
        RenderPassSlot::PostProcess,
        RenderPassSlot::PostProcess,
        RenderPassSlot::Present,
    };
    const auto duplicateResult = validateRenderPipelineContracts(duplicate);
    CHECK(!duplicateResult.valid());
    CHECK(duplicateResult.errors.front().code ==
          RenderPipelineContractErrorCode::DuplicateSlot);
    CHECK(duplicateResult.errors.front().passIndex == 1u);

    constexpr std::array unknown = {
        static_cast<RenderPassSlot>(255u),
    };
    const auto unknownResult = validateRenderPipelineContracts(unknown);
    CHECK(!unknownResult.valid());
    CHECK(unknownResult.errors.front().code ==
          RenderPipelineContractErrorCode::UnknownSlot);
}

TEST_CASE(temporal_and_overlay_nodes_require_their_structural_inputs)
{
    constexpr std::array taaWithoutGbuffer = {
        RenderPassSlot::PostProcess,
        RenderPassSlot::TAA,
        RenderPassSlot::Present,
    };
    const auto taaResult = validateRenderPipelineContracts(taaWithoutGbuffer);
    CHECK(!taaResult.valid());
    CHECK(taaResult.errors.front().resource ==
          RenderPassResourceId::GBufferWorldPosition);

    constexpr std::array overlayBeforePresent = {
        RenderPassSlot::EditorOverlay,
        RenderPassSlot::PostProcess,
        RenderPassSlot::Present,
    };
    const auto overlayResult =
        validateRenderPipelineContracts(overlayBeforePresent);
    CHECK(!overlayResult.valid());
    CHECK(overlayResult.errors.front().resource ==
          RenderPassResourceId::PresentedColor);
}

TEST_CASE(renderer_rejects_invalid_descriptor_without_tearing_down_baseline)
{
    Renderer renderer;
    const std::vector<RenderPassSlot> baseline = renderer.pipelineDesc().passes;
    RenderPipelineDesc invalid{{RenderPassSlot::BloomBlur}};

    renderer.configurePipeline(invalid);

    CHECK(renderer.pipelineDesc().passes == baseline);
}

TEST_CASE(optional_producers_may_be_absent_but_must_not_execute_late)
{
    const std::vector<std::vector<RenderPassSlot>> invalid = {
        {RenderPassSlot::PostProcess, RenderPassSlot::Present, RenderPassSlot::FXAA},
        {RenderPassSlot::PostProcess, RenderPassSlot::ColorGrading,
         RenderPassSlot::SMAA, RenderPassSlot::Present},
        {RenderPassSlot::GBuffer, RenderPassSlot::Lighting, RenderPassSlot::SSAO},
        {RenderPassSlot::GBuffer, RenderPassSlot::Lighting, RenderPassSlot::Skybox},
        {RenderPassSlot::GBuffer, RenderPassSlot::PostProcess,
         RenderPassSlot::TAA, RenderPassSlot::MotionVector, RenderPassSlot::Present},
    };
    for (const auto& slots : invalid) {
        const auto result = validateRenderPipelineContracts(slots);
        CHECK(!result.valid());
        CHECK(std::any_of(result.errors.begin(), result.errors.end(), [](const auto& e) {
            return e.code == RenderPipelineContractErrorCode::OptionalProducerAfterConsumer;
        }));
        Renderer renderer;
        const auto baseline = renderer.pipelineDesc().passes;
        renderer.configurePipeline(RenderPipelineDesc{slots});
        CHECK(renderer.pipelineDesc().passes == baseline);
    }
    const std::array absent = {RenderPassSlot::PostProcess, RenderPassSlot::Present};
    CHECK(validateRenderPipelineContracts(absent).valid());
    const std::array ordered = {RenderPassSlot::GBuffer, RenderPassSlot::Skybox,
        RenderPassSlot::SSAO, RenderPassSlot::Lighting,
        RenderPassSlot::PostProcess, RenderPassSlot::FXAA, RenderPassSlot::Present};
    CHECK(validateRenderPipelineContracts(ordered).valid());
}

TEST_SUITE_END
