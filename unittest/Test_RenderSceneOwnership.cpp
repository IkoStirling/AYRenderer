#include "AYRenderer/RenderScene.h"
#include "AYRenderer/SceneVisibility.h"
#include "AYRenderer/RendererSubSystem.h"
#include "AYTest.h"

#include <utility>
#include <cstring>
#include <future>

using namespace ayt::render;
using ayt::math::Float4x4;

TEST_SUITE(RendererSceneOwnershipTests)

TEST_CASE(owned_data_is_captured_before_producer_scope_ends)
{
    RenderScene scene(RenderScene::DrawDataOwnership::OwnedCpu);
    {
        Float4x4 bones[3] = {Float4x4::identity(), Float4x4::identity(), Float4x4::identity()};
        uint32_t remap[2] = {2u, 0u};
        DrawPayload2D payload;
        payload.packedSortKey = 71u;
        ParticleDrawData particles;
        particles.vertices.push_back({1, 2, 3, 0, 0, 1, 1, 1, 1});
        particles.indices = {0, 1, 2};
        DrawItem item;
        item.boneMatrices = bones;
        item.jointCount = 2u;
        item.skeletonJointCount = 3u;
        item.boneRemap = remap;
        item.payload = &payload;
        item.particleBatch = &particles;
        item.motionObjectId = 79u;
        scene.add(item);
        CHECK(scene.items()[0].boneMatrices != bones);
        CHECK(scene.items()[0].boneRemap != remap);
        CHECK(scene.items()[0].payload != &payload);
        CHECK(scene.items()[0].particleBatch != &particles);
        remap[0] = 1u;
        payload.packedSortKey = 99u;
        particles.vertices.clear();
        particles.indices.clear();
    }
    const auto& item = scene.items()[0];
    CHECK_INT_EQ(item.boneRemap[0], 2u);
    CHECK_INT_EQ(item.payload->packedSortKey, 71u);
    const auto identity = Float4x4::identity();
    CHECK(std::memcmp(&item.boneMatrices[2], &identity, sizeof(identity)) == 0);
    CHECK_INT_EQ(item.particleBatch->vertices.size(), 1u);
    CHECK_INT_EQ(item.particleBatch->indices.size(), 3u);
    CHECK(item.particleBatch->vertices[0].z == 3.0f);
    CHECK(item.motionObjectId == 79u);
    CHECK_FALSE(scene.hasBorrowedGpuStreams());
}

TEST_CASE(copy_move_and_source_clear_retain_stable_backing)
{
    RenderScene source(RenderScene::DrawDataOwnership::OwnedCpu);
    DrawPayload2D payload;
    payload.packedSortKey = 19u;
    DrawItem item;
    item.payload = &payload;
    source.add(item);
    RenderScene copy = source;
    const auto* stable = copy.items()[0].payload;
    source.clear();
    for (int i = 0; i < 128; ++i) copy.add(item);
    CHECK(copy.items()[0].payload == stable);
    RenderScene moved = std::move(copy);
    CHECK_INT_EQ(moved.items()[0].payload->packedSortKey, 19u);
    copy.clear();
    RenderScene assigned;
    assigned = moved;
    moved.clear();
    CHECK(assigned.drawDataOwnership() == RenderScene::DrawDataOwnership::OwnedCpu);
    CHECK_INT_EQ(assigned.items()[0].payload->packedSortKey, 19u);
    assigned.clear();
    CHECK(assigned.empty());
    CHECK(assigned.drawDataOwnership() == RenderScene::DrawDataOwnership::OwnedCpu);
}

TEST_CASE(borrowed_compatibility_and_owned_overloads)
{
    DrawPayload2D payload;
    DrawItem item;
    item.payload = &payload;
    RenderScene legacy;
    legacy.add(item);
    CHECK(legacy.items()[0].payload == &payload);
    Float4x4 bone = Float4x4::identity();
    RenderScene owned(RenderScene::DrawDataOwnership::OwnedCpu);
    owned.add(MeshHandle{}, MaterialHandle{}, Float4x4::identity(), &bone, 1u);
    CHECK(owned.items()[0].boneMatrices != &bone);
    owned.add(MeshHandle{}, MaterialHandle{});
    CHECK(owned.items()[1].boneMatrices == nullptr);
}

TEST_CASE(gpu_stream_is_explicitly_borrowed_even_in_owned_cpu_mode)
{
    RenderScene owned(RenderScene::DrawDataOwnership::OwnedCpu);
    int stream = 0;
    ParticleDrawData gpu;
    gpu.gpuStream = &stream;
    gpu.submitGpu = [](void*, uint16_t) {};
    DrawItem item;
    item.particleBatch = &gpu;
    owned.add(item);
    CHECK(owned.hasBorrowedGpuStreams());
    CHECK(owned.items()[0].particleBatch->gpuStream == &stream);
    owned.clear();
    CHECK_FALSE(owned.hasBorrowedGpuStreams());
}

TEST_CASE(visibility_copy_can_outlive_its_owned_source)
{
    RenderScene source(RenderScene::DrawDataOwnership::OwnedCpu);
    DrawPayload2D payload;
    payload.packedSortKey = 11u;
    DrawItem item;
    item.payload = &payload;
    source.add(item);
    source.add(DrawItem{});
    source.setOverlayCamera2D(Float4x4::identity(), Float4x4::identity(), 0x35u);
    RenderScene filtered(RenderScene::DrawDataOwnership::OwnedCpu);
    copyVisibleSceneForPresentation(source, filtered, {false, false, true});
    source.clear();
    CHECK_INT_EQ(filtered.items().size(), 1u);
    CHECK_INT_EQ(filtered.items()[0].payload->packedSortKey, 11u);
    CHECK_INT_EQ(filtered.overlayCamera2D().layerMask, 0x35u);
}

TEST_CASE(subsystem_captures_owned_data_by_default)
{
    RendererSubSystem subsystem;
    CHECK(subsystem.renderScene().drawDataOwnership() == RenderScene::DrawDataOwnership::OwnedCpu);
}

TEST_CASE(cpu_snapshot_can_be_consumed_on_another_thread_after_source_clear)
{
    RenderScene source(RenderScene::DrawDataOwnership::OwnedCpu);
    DrawPayload2D payload;
    payload.packedSortKey = 57u;
    DrawItem item;
    item.payload = &payload;
    source.add(item);
    auto snapshot = std::make_shared<const RenderScene>(source);
    std::promise<void> ready;
    auto go = ready.get_future();
    auto result = std::async(std::launch::async, [snapshot, go = std::move(go)]() mutable {
        go.wait();
        return snapshot->items()[0].payload->packedSortKey;
    });
    source.clear();
    payload.packedSortKey = 999u;
    ready.set_value();
    CHECK_INT_EQ(result.get(), 57u);
}

TEST_CASE(empty_data_and_self_assignment_preserve_scene_invariants)
{
    RenderScene scene(RenderScene::DrawDataOwnership::OwnedCpu);
    Float4x4 emptyBones = Float4x4::identity();
    uint32_t emptyRemap = 0u;
    DrawItem item;
    item.boneMatrices = &emptyBones;
    item.boneRemap = &emptyRemap;
    scene.add(item);
    scene = scene;
    CHECK_INT_EQ(scene.items().size(), 1u);
    CHECK(scene.items()[0].boneMatrices == nullptr);
    CHECK(scene.items()[0].boneRemap == nullptr);
    CHECK_FALSE(scene.hasBorrowedGpuStreams());
    ParticleDrawData callbackOnly;
    callbackOnly.submitGpu = [](void*, uint16_t) {};
    item = {};
    item.particleBatch = &callbackOnly;
    scene.add(item);
    CHECK(scene.hasBorrowedGpuStreams());
}

TEST_SUITE_END
