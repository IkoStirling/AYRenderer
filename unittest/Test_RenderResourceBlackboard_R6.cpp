#include "AYTest.h"

#include "detail/RenderResourceBlackboard.h"

#include <string>

using namespace ayt::render::detail;

TEST_SUITE(AYRenderer_ResourceBlackboard_R6)

TEST_CASE(frame_production_resets_but_persistent_history_survives)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    blackboard.publish(
        BlackboardResourceId::TaaHistoryRead,
        BlackboardResourceLifetime::PersistentHistory,
        bgfx::FrameBufferHandle{7}, BGFX_INVALID_HANDLE,
        1280, 720, 3, true);
    blackboard.markProduced(BlackboardResourceId::TaaHistoryRead);
    CHECK(blackboard.producedThisFrame(
        BlackboardResourceId::TaaHistoryRead));

    blackboard.beginFrame();
    const BlackboardResourceEntry* history = blackboard.find(
        BlackboardResourceId::TaaHistoryRead);
    CHECK(history != nullptr);
    CHECK(history->contentValid);
    CHECK_FALSE(blackboard.producedThisFrame(
        BlackboardResourceId::TaaHistoryRead));
    CHECK(history->generation == 3u);
}

TEST_CASE(current_frame_motion_expires_at_the_next_frame_boundary)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    blackboard.publish(
        BlackboardResourceId::MotionVectors,
        BlackboardResourceLifetime::External,
        bgfx::FrameBufferHandle{8}, bgfx::TextureHandle{9},
        640, 360, 4, false);
    blackboard.markProduced(BlackboardResourceId::MotionVectors);
    CHECK(blackboard.producedThisFrame(
        BlackboardResourceId::MotionVectors));

    blackboard.beginFrame();
    const BlackboardResourceEntry* motion = blackboard.find(
        BlackboardResourceId::MotionVectors);
    CHECK(motion != nullptr);
    CHECK_FALSE(motion->contentValid);
    CHECK(motion->invalidationReason
          == ResourceInvalidationReason::AwaitingProducer);
}

TEST_CASE(temporal_invalidation_records_one_explicit_reason)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    blackboard.publish(
        BlackboardResourceId::TaaHistoryRead,
        BlackboardResourceLifetime::PersistentHistory,
        bgfx::FrameBufferHandle{10}, BGFX_INVALID_HANDLE,
        800, 600, 5, true);
    blackboard.publish(
        BlackboardResourceId::TaaHistoryWrite,
        BlackboardResourceLifetime::PersistentHistory,
        bgfx::FrameBufferHandle{11}, BGFX_INVALID_HANDLE,
        800, 600, 5, false);

    blackboard.invalidateTemporal(ResourceInvalidationReason::Resize);
    for (const BlackboardResourceId id : {
             BlackboardResourceId::TaaHistoryRead,
             BlackboardResourceId::TaaHistoryWrite}) {
        const BlackboardResourceEntry* entry = blackboard.find(id);
        CHECK(entry != nullptr);
        CHECK_FALSE(entry->contentValid);
        CHECK(entry->invalidationReason
              == ResourceInvalidationReason::Resize);
    }
    CHECK(std::string(resourceInvalidationReasonName(
              ResourceInvalidationReason::Resize)) == "resize");
}

TEST_CASE(gbuffer_resolves_only_as_one_coherent_current_frame_set)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    blackboard.publishGBuffer(
        bgfx::FrameBufferHandle{20},
        bgfx::TextureHandle{21}, bgfx::TextureHandle{22},
        bgfx::TextureHandle{23}, bgfx::TextureHandle{24},
        bgfx::TextureHandle{25}, 1280, 720, 6, true);

    BlackboardGBufferView view;
    CHECK(blackboard.resolveGBuffer(view));
    CHECK(view.framebuffer.idx == 20u);
    CHECK(view.albedo.idx == 21u);
    CHECK(view.normal.idx == 22u);
    CHECK(view.worldPosition.idx == 23u);
    CHECK(view.material.idx == 24u);
    CHECK(view.depth.idx == 25u);
    CHECK(view.width == 1280u);
    CHECK(view.height == 720u);
    CHECK(view.generation == 6u);

    blackboard.beginFrame();
    CHECK_FALSE(blackboard.resolveGBuffer(view));
}

TEST_CASE(gbuffer_rejects_mixed_generation_attachments)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    blackboard.publishGBuffer(
        bgfx::FrameBufferHandle{30},
        bgfx::TextureHandle{31}, bgfx::TextureHandle{32},
        bgfx::TextureHandle{33}, bgfx::TextureHandle{34},
        bgfx::TextureHandle{35}, 800, 600, 9, true);
    blackboard.publishProduced(
        BlackboardResourceId::GBufferDepth,
        BlackboardResourceLifetime::External,
        bgfx::FrameBufferHandle{30}, bgfx::TextureHandle{35},
        800, 600, 10);

    BlackboardGBufferView view;
    CHECK_FALSE(blackboard.resolveGBuffer(view));
}

TEST_CASE(transient_ssao_and_haze_expire_at_frame_boundary)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    blackboard.publishProduced(
        BlackboardResourceId::SsaoOcclusion,
        BlackboardResourceLifetime::Transient,
        bgfx::FrameBufferHandle{40}, bgfx::TextureHandle{41},
        640, 360, 0);
    blackboard.publishProduced(
        BlackboardResourceId::DepthHazeColor,
        BlackboardResourceLifetime::Transient,
        bgfx::FrameBufferHandle{42}, bgfx::TextureHandle{43},
        640, 360, 0);
    CHECK(blackboard.findProduced(
        BlackboardResourceId::SsaoOcclusion) != nullptr);
    CHECK(blackboard.findProduced(
        BlackboardResourceId::DepthHazeColor) != nullptr);

    blackboard.beginFrame();
    CHECK(blackboard.findProduced(
        BlackboardResourceId::SsaoOcclusion) == nullptr);
    CHECK(blackboard.findProduced(
        BlackboardResourceId::DepthHazeColor) == nullptr);
}

TEST_CASE(scene_color_and_bloom_outputs_share_current_frame_contract)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    const struct Output final {
        BlackboardResourceId id;
        BlackboardResourceLifetime lifetime;
        uint16_t framebuffer;
        uint16_t texture;
    } outputs[] = {
        {BlackboardResourceId::LightingColor,
         BlackboardResourceLifetime::External, 50, 51},
        {BlackboardResourceId::SkyboxColor,
         BlackboardResourceLifetime::External, 52, 53},
        {BlackboardResourceId::BloomBright,
         BlackboardResourceLifetime::Transient, 54, 55},
        {BlackboardResourceId::BloomBlurA,
         BlackboardResourceLifetime::Transient, 56, 57},
        {BlackboardResourceId::BloomBlurB,
         BlackboardResourceLifetime::Transient, 58, 59},
    };
    for (const Output& output : outputs) {
        blackboard.publishProduced(
            output.id, output.lifetime,
            bgfx::FrameBufferHandle{output.framebuffer},
            bgfx::TextureHandle{output.texture},
            640, 360, 2);
        CHECK(blackboard.findProduced(output.id) != nullptr);
    }

    blackboard.beginFrame();
    for (const Output& output : outputs) {
        CHECK(blackboard.findProduced(output.id) == nullptr);
    }
}

TEST_CASE(frame_output_invalidation_covers_scene_color_and_bloom)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    for (const BlackboardResourceId id : {
             BlackboardResourceId::LightingColor,
             BlackboardResourceId::SkyboxColor,
             BlackboardResourceId::BloomBright,
             BlackboardResourceId::BloomBlurA,
             BlackboardResourceId::BloomBlurB}) {
        blackboard.publishProduced(
            id, BlackboardResourceLifetime::Transient,
            bgfx::FrameBufferHandle{60}, bgfx::TextureHandle{61},
            320, 180, 0);
    }

    blackboard.invalidateFrameOutputs(ResourceInvalidationReason::Resize);
    for (const BlackboardResourceId id : {
             BlackboardResourceId::LightingColor,
             BlackboardResourceId::SkyboxColor,
             BlackboardResourceId::BloomBright,
             BlackboardResourceId::BloomBlurA,
             BlackboardResourceId::BloomBlurB}) {
        const BlackboardResourceEntry* entry = blackboard.find(id);
        CHECK(entry != nullptr);
        CHECK_FALSE(entry->contentValid);
        CHECK(entry->invalidationReason
              == ResourceInvalidationReason::Resize);
    }
}

TEST_CASE(diagnostic_summary_counts_declared_valid_and_produced_resources)
{
    RenderResourceBlackboard blackboard;
    blackboard.beginFrame();
    blackboard.publishProduced(
        BlackboardResourceId::MotionVectors,
        BlackboardResourceLifetime::External,
        bgfx::FrameBufferHandle{81}, bgfx::TextureHandle{82},
        1280, 720, 3);
    blackboard.publish(
        BlackboardResourceId::TaaHistoryRead,
        BlackboardResourceLifetime::PersistentHistory,
        bgfx::FrameBufferHandle{83}, bgfx::TextureHandle{84},
        1280, 720, 4,
        true);
    blackboard.publish(
        BlackboardResourceId::BloomBright,
        BlackboardResourceLifetime::Transient,
        BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, 640, 360, 0, false,
        ResourceInvalidationReason::PreparationFailed);

    const BlackboardDiagnosticStats stats = blackboard.diagnosticStats();
    CHECK(stats.declaredResources == 3u);
    CHECK(stats.availableResources == 2u);
    CHECK(stats.validResources == 2u);
    CHECK(stats.producedResources == 1u);
    CHECK(stats.invalidResources == 1u);
    CHECK(stats.persistentHistoryResources == 1u);
}

TEST_SUITE_END
