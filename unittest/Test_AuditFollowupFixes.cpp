#include "AYTest.h"

#include "AYRenderer/RenderScene.h"
#include "detail/RenderPass.h"
#include "detail/ShadowAtlas.h"

#include <cstdint>

using ayt::render::Light;
using ayt::render::LightType;
using ayt::render::SceneLights;
using ayt::render::detail::RateLimitedLogTable;
using ayt::render::detail::computeShadowLightOrder;

TEST_SUITE(AuditFollowup_ShadowLightOrder)

TEST_CASE(non_shadowing_prefix_does_not_steal_atlas_slot)
{
    SceneLights lights;

    Light unshadowed = Light::directional(
        ayt::math::FVector3(1.0f, -1.0f, 0.0f),
        ayt::math::FVector3(1.0f, 0.0f, 0.0f));
    unshadowed.castShadow = false;
    lights.add(unshadowed); // source slot 0

    Light caster = Light::directional(
        ayt::math::FVector3(-1.0f, -1.0f, 0.0f),
        ayt::math::FVector3(0.0f, 1.0f, 0.0f));
    caster.castShadow = true;
    lights.add(caster); // source slot 1

    const auto order = computeShadowLightOrder(lights);
    CHECK(order.lightCount == 2u);
    CHECK(order.shadowCasterCount == 1u);
    CHECK(order.indices[0] == 1u);
    CHECK(order.indices[1] == 0u);
}

TEST_CASE(point_shadow_request_stays_in_unshadowed_suffix)
{
    SceneLights lights;

    Light point;
    point.type = LightType::Point;
    point.castShadow = true;
    lights.add(point); // unsupported omni caster

    Light spot;
    spot.type = LightType::Spot;
    spot.castShadow = true;
    lights.add(spot);

    const auto order = computeShadowLightOrder(lights);
    CHECK(order.lightCount == 2u);
    CHECK(order.shadowCasterCount == 1u);
    CHECK(order.indices[0] == 1u); // supported Spot owns atlas slot 0
    CHECK(order.indices[1] == 0u); // Point remains lit but unshadowed
}

TEST_CASE(stable_order_is_preserved_inside_both_partitions)
{
    SceneLights lights;
    for (uint32_t i = 0; i < 4u; ++i) {
        Light light;
        light.castShadow = (i == 1u || i == 3u);
        lights.add(light);
    }

    const auto order = computeShadowLightOrder(lights);
    CHECK(order.shadowCasterCount == 2u);
    CHECK(order.indices[0] == 1u);
    CHECK(order.indices[1] == 3u);
    CHECK(order.indices[2] == 0u);
    CHECK(order.indices[3] == 2u);
}

TEST_SUITE_END

TEST_SUITE(AuditFollowup_RateLimitedLogTable)

TEST_CASE(first_occurrence_is_independent_per_pass_and_reason)
{
    RateLimitedLogTable table;
    CHECK(table.shouldEmit("PassA", "ReasonA", 4u));
    CHECK(!table.shouldEmit("PassA", "ReasonA", 4u));
    CHECK(table.shouldEmit("PassB", "ReasonA", 4u));
    CHECK(table.shouldEmit("PassA", "ReasonB", 4u));
}

TEST_CASE(each_key_reemits_on_its_own_interval)
{
    RateLimitedLogTable table;
    CHECK(table.shouldEmit("PassA", "Reason", 3u)); // count 0
    CHECK(!table.shouldEmit("PassA", "Reason", 3u));
    CHECK(!table.shouldEmit("PassA", "Reason", 3u));
    CHECK(table.shouldEmit("PassA", "Reason", 3u)); // count 3

    // Activity on A does not advance B.
    CHECK(table.shouldEmit("PassB", "Reason", 3u));
    CHECK(!table.shouldEmit("PassB", "Reason", 3u));
}

TEST_CASE(null_names_are_safe_and_have_a_stable_key)
{
    RateLimitedLogTable table;
    CHECK(table.shouldEmit(nullptr, nullptr, 2u));
    CHECK(!table.shouldEmit(nullptr, nullptr, 2u));
    CHECK(table.shouldEmit(nullptr, nullptr, 2u));
}

TEST_SUITE_END
