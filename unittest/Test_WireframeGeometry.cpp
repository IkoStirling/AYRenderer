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
