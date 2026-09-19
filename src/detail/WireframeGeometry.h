#pragma once

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ayt::render::detail
{

struct WireframePosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

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

// Preserve the six-index-per-triangle layout used by sub-mesh range mapping,
// while replacing a shared coplanar edge with a degenerate line. This hides
// triangulation diagonals on planar quad faces without changing mesh formats.
template <typename Index>
std::vector<Index> buildFeatureWireframeIndices(
    const Index* indices, uint32_t indexCount,
    const WireframePosition* positions, uint32_t vertexCount)
{
    std::vector<Index> lines = buildTriangleWireframeIndices(indices, indexCount);
    if (indices == nullptr || positions == nullptr || vertexCount == 0u) {
        return lines;
    }

    struct QuantizedPoint {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        bool operator==(const QuantizedPoint&) const = default;
        bool operator<(const QuantizedPoint& other) const noexcept {
            if (x != other.x) return x < other.x;
            if (y != other.y) return y < other.y;
            return z < other.z;
        }
    };
    struct EdgeKey {
        QuantizedPoint a;
        QuantizedPoint b;
        bool operator==(const EdgeKey&) const = default;
    };
    struct EdgeHash {
        std::size_t operator()(const EdgeKey& edge) const noexcept {
            std::size_t value = 1469598103934665603ull;
            const auto mix = [&value](std::int64_t part) {
                value ^= std::hash<std::int64_t>{}(part);
                value *= 1099511628211ull;
            };
            mix(edge.a.x); mix(edge.a.y); mix(edge.a.z);
            mix(edge.b.x); mix(edge.b.y); mix(edge.b.z);
            return value;
        }
    };
    struct EdgeReference {
        std::size_t lineOffset = 0;
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 0.0f;
    };

    constexpr double kWeldScale = 100000.0;
    const auto quantize = [](const WireframePosition& point) {
        return QuantizedPoint{
            static_cast<std::int64_t>(std::llround(point.x * kWeldScale)),
            static_cast<std::int64_t>(std::llround(point.y * kWeldScale)),
            static_cast<std::int64_t>(std::llround(point.z * kWeldScale))};
    };
    const auto edgeKey = [&quantize](const WireframePosition& first,
                                     const WireframePosition& second) {
        QuantizedPoint a = quantize(first);
        QuantizedPoint b = quantize(second);
        if (b < a) std::swap(a, b);
        return EdgeKey{a, b};
    };

    std::unordered_map<EdgeKey, std::vector<EdgeReference>, EdgeHash> edges;
    edges.reserve(static_cast<std::size_t>(indexCount));
    for (uint32_t index = 0; index + 2u < indexCount; index += 3u) {
        const uint32_t ia = static_cast<uint32_t>(indices[index]);
        const uint32_t ib = static_cast<uint32_t>(indices[index + 1u]);
        const uint32_t ic = static_cast<uint32_t>(indices[index + 2u]);
        if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount) {
            return buildTriangleWireframeIndices(indices, indexCount);
        }
        const WireframePosition& a = positions[ia];
        const WireframePosition& b = positions[ib];
        const WireframePosition& c = positions[ic];
        const float abx = b.x - a.x;
        const float aby = b.y - a.y;
        const float abz = b.z - a.z;
        const float acx = c.x - a.x;
        const float acy = c.y - a.y;
        const float acz = c.z - a.z;
        float nx = aby * acz - abz * acy;
        float ny = abz * acx - abx * acz;
        float nz = abx * acy - aby * acx;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!std::isfinite(length) || length <= 1.0e-8f) continue;
        nx /= length;
        ny /= length;
        nz /= length;

        const std::size_t lineBase = static_cast<std::size_t>(index / 3u) * 6u;
        edges[edgeKey(a, b)].push_back({lineBase, nx, ny, nz});
        edges[edgeKey(b, c)].push_back({lineBase + 2u, nx, ny, nz});
        edges[edgeKey(c, a)].push_back({lineBase + 4u, nx, ny, nz});
    }

    constexpr float kCoplanarNormalDot = 0.99985f;
    for (const auto& [key, refs] : edges) {
        (void)key;
        if (refs.size() != 2u) continue;
        const float dot = refs[0].nx * refs[1].nx
            + refs[0].ny * refs[1].ny + refs[0].nz * refs[1].nz;
        if (std::fabs(dot) < kCoplanarNormalDot) continue;
        for (const EdgeReference& ref : refs) {
            lines[ref.lineOffset + 1u] = lines[ref.lineOffset];
        }
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
