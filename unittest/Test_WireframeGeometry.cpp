#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/WireframeGeometry.h"

#include <cstdint>
#include <iterator>

using namespace ayt::render::detail;

TEST_SUITE(WireframeGeometryTests)

TEST_CASE(triangle_indices_expand_to_independent_edges)
{
    const std::uint16_t triangles[] = {0, 1, 2, 2, 3, 0};
    const auto lines = buildTriangleWireframeIndices(
        triangles, static_cast<std::uint32_t>(std::size(triangles)));
    const std::uint16_t expected[] = {
        0, 1, 1, 2, 2, 0,
        2, 3, 3, 0, 0, 2,
    };
    CHECK(lines.size() == std::size(expected));
    for (std::size_t index = 0; index < lines.size(); ++index) {
        CHECK(lines[index] == expected[index]);
    }
}

TEST_CASE(coplanar_triangle_diagonal_is_hidden)
{
    const WireframePosition positions[] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    const std::uint16_t triangles[] = {0, 1, 2, 0, 2, 3};
    const auto lines = buildFeatureWireframeIndices(
        triangles, static_cast<std::uint32_t>(std::size(triangles)),
        positions, static_cast<std::uint32_t>(std::size(positions)));

    CHECK(lines.size() == 12u);
    if (lines.size() != 12u) return;
    CHECK(lines[5] == lines[4]);
    CHECK(lines[7] == lines[6]);
    CHECK(lines[0] != lines[1]);
    CHECK(lines[2] != lines[3]);
    CHECK(lines[8] != lines[9]);
    CHECK(lines[10] != lines[11]);
}

TEST_CASE(sharp_shared_edge_remains_visible)
{
    const WireframePosition positions[] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    const std::uint16_t triangles[] = {0, 1, 2, 0, 3, 1};
    const auto lines = buildFeatureWireframeIndices(
        triangles, static_cast<std::uint32_t>(std::size(triangles)),
        positions, static_cast<std::uint32_t>(std::size(positions)));

    CHECK(lines.size() == 12u);
    if (lines.size() != 12u) return;
    CHECK(lines[0] != lines[1]);
    CHECK(lines[10] != lines[11]);
}

TEST_CASE(submesh_triangle_ranges_map_to_wireframe_ranges)
{
    const WireframeIndexRange range = resolveWireframeIndexRange(3, 6, 18);
    CHECK_TRUE(range.valid);
    CHECK(range.firstIndex == 6u);
    CHECK(range.indexCount == 12u);

    CHECK_FALSE(resolveWireframeIndexRange(1, 3, 18).valid);
    CHECK_FALSE(resolveWireframeIndexRange(3, 4, 18).valid);
    CHECK_FALSE(resolveWireframeIndexRange(6, 6, 16).valid);
}

TEST_CASE(renderer_keeps_wireframe_as_scene_dispatch_state)
{
    ayt::render::Renderer renderer;
    CHECK_FALSE(renderer.isWireframeEnabled());
    renderer.setWireframeEnabled(true);
    CHECK_TRUE(renderer.isWireframeEnabled());
    CHECK_FALSE(renderer.isDebugOverlayEnabled());
    renderer.setWireframeEnabled(false);
    CHECK_FALSE(renderer.isWireframeEnabled());
}

TEST_SUITE_END
