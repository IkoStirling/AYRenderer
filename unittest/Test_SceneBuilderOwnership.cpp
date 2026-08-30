#include "AYRenderer/RendererSubSystem.h"
#include "AYTest.h"

using namespace ayt::render;

TEST_SUITE(RendererSceneBuilderOwnershipTests)

TEST_CASE(world_owned_scene_builders_detach_as_one_group)
{
    RendererSubSystem renderer;
    int editWorld = 0;
    int playWorld = 0;
    int globalCalls = 0;
    int editCalls = 0;
    int playCalls = 0;

    renderer.setSceneBuilder([&](RenderScene&) { ++globalCalls; });
    renderer.addSceneBuilderForOwner(
        &editWorld, [&](RenderScene&) { ++editCalls; });
    renderer.addSceneBuilderForOwner(
        &editWorld, [&](RenderScene&) { ++editCalls; });
    renderer.addSceneBuilderForOwner(
        &playWorld, [&](RenderScene&) { ++playCalls; });

    renderer.update(0.0f);
    CHECK(globalCalls == 1);
    CHECK(editCalls == 2);
    CHECK(playCalls == 1);

    renderer.clearSceneBuildersForOwner(&editWorld);
    renderer.update(0.0f);
    CHECK(globalCalls == 2);
    CHECK(editCalls == 2);
    CHECK(playCalls == 2);

    renderer.clearSceneBuildersForOwner(&playWorld);
    renderer.update(0.0f);
    CHECK(globalCalls == 3);
    CHECK(editCalls == 2);
    CHECK(playCalls == 2);

    renderer.shutdown();
}

TEST_CASE(clear_during_dispatch_skips_later_revoked_callbacks)
{
    RendererSubSystem renderer;
    int editWorld = 0;
    int firstCalls = 0;
    int revokedCalls = 0;

    renderer.addSceneBuilderForOwner(
        &editWorld,
        [&](RenderScene&) {
            ++firstCalls;
            renderer.clearSceneBuildersForOwner(&editWorld);
        });
    renderer.addSceneBuilderForOwner(
        &editWorld, [&](RenderScene&) { ++revokedCalls; });

    renderer.update(0.0f);
    CHECK(firstCalls == 1);
    CHECK(revokedCalls == 0);

    renderer.update(0.0f);
    CHECK(firstCalls == 1);
    CHECK(revokedCalls == 0);

    renderer.shutdown();
}

TEST_SUITE_END
