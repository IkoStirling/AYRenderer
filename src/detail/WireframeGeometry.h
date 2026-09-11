#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayt::render::detail
{

// Convert triangle-list indices into an explicit line list. Keeping this as a
// second mesh buffer lets the Scene View request wireframe per dispatch;
// bgfx's debug wireframe flag is process-wide and would also affect UI and
// unrelated renderer views.
template <typename Index>
std::vector<Index> buildTriangleWireframeIndices(const Index* indices,
                                                  uint32_t indexCount)
{
    std::vector<Index> lines;
    if (indices == nullptr) return lines;
    lines.reserve(static_cast<std::size_t>(indexCount / 3u) * 6u);
    for (uint32_t index = 0; index + 2u < indexCount; index += 3u) {
        const Index a = indices[index];
        const Index b = indices[index + 1u];
        const Index c = indices[index + 2u];
        lines.insert(lines.end(), {a, b, b, c, c, a});
    }
    return lines;
}

struct WireframeIndexRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    bool valid = false;
};

inline WireframeIndexRange resolveWireframeIndexRange(
    uint32_t triangleFirstIndex, uint32_t triangleIndexCount,
    uint32_t wireframeIndexCount) noexcept
{
    if (triangleIndexCount == 0u || triangleFirstIndex % 3u != 0u
        || triangleIndexCount % 3u != 0u) {
        return {};
    }
    const uint64_t first = static_cast<uint64_t>(triangleFirstIndex) * 2u;
    const uint64_t count = static_cast<uint64_t>(triangleIndexCount) * 2u;
    if (first + count > wireframeIndexCount) return {};
    return {static_cast<uint32_t>(first), static_cast<uint32_t>(count), true};
}

} // namespace ayt::render::detail
