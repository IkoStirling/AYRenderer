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

TEST_SUITE_END
