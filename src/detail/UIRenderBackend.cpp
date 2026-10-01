#include "AYRenderer/UIRenderBackend.h"

#include "AYRenderer.h"
#include "detail/BgfxFontAtlas.h"
#include "detail/BGFXAdapter.h"
#include "detail/RenderTargetPool.h"
#include "detail/UiGpuContext.h"
#include "AYShader/ShaderResourcePool.h"

#include <AYCore/CoreUtility.h>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ayt::render {

namespace {

uint32_t nextTextCodePoint(const std::wstring& text, size_t& index)
{
    const uint32_t first = static_cast<uint32_t>(text[index++]);
    if constexpr (sizeof(wchar_t) == 2) {
        if (first >= 0xD800u && first <= 0xDBFFu && index < text.size()) {
            const uint32_t second = static_cast<uint32_t>(text[index]);
            if (second >= 0xDC00u && second <= 0xDFFFu) {
                ++index;
                return 0x10000u + ((first - 0xD800u) << 10u) + (second - 0xDC00u);
            }
        }
        if (first >= 0xD800u && first <= 0xDFFFu) return 0xFFFDu;
    }
    return first;
}

struct UiVertex {
    float    x;
    float    y;
    float    z;
    uint32_t abgr;
    float    u;
    float    v;
    // SDF only: shape center + half-extent (replaces the u_rect uniform so
    // same-param SDF items can share a submission). Flat/text vertices
    // leave these 0 — their shaders never read TexCoord1.
    float    shapeCx;
    float    shapeCy;
    float    shapeHalfW;
    float    shapeHalfH;
    // SDF only: clip rect as (center, half-extent), soft-clip seam AA.
    // The draw quad is already CPU-intersected with the clip; this lets
    // the SDF shader fade coverage across the clip edge instead of
    // hard-cutting stroke/shadow AA gradients (scroll edges). Flat/text
    // vertices leave these 0 — their shaders never read TexCoord2.
    float    clipCx;
    float    clipCy;
    float    clipHalfW;
    float    clipHalfH;
};

struct UiTriangleMesh {
    std::vector<ayt::math::FVector2> positions;
    std::vector<uint32_t> indices;
};

struct UiPathContour {
    std::vector<ayt::math::FVector2> points;
    ayt::ui::PathWinding winding = ayt::ui::PathWinding::CounterClockwise;
    bool closed = true;
};

struct UiPathData {
    std::vector<UiPathContour> contours;
    ayt::math::FVector4 fillColor{1.0f, 1.0f, 1.0f, 1.0f};
    ayt::math::FVector4 strokeColor{1.0f, 1.0f, 1.0f, 1.0f};
    float strokeWidth = 1.0f;
    ayt::ui::PathStrokeCap strokeCap = ayt::ui::PathStrokeCap::Butt;
    ayt::ui::PathStrokeJoin strokeJoin = ayt::ui::PathStrokeJoin::Miter;
    float miterLimit = 4.0f;
};

struct UiPathFillContour {
    UiTriangleMesh mesh;
    ayt::ui::PathWinding winding = ayt::ui::PathWinding::CounterClockwise;
};

struct UiPathSnapshot {
    std::vector<UiPathFillContour> fillContours;
    UiTriangleMesh strokeMesh;
    ayt::math::FRectangle bounds;
    bool hasBounds = false;
};

// P0 unified batch: every draw entry becomes one UiItem appended to
// frame.items in submission order (array order == painter order). flush()
// may derive a safe logical order, but never mutates this source stream.
// Sdf items arrive in P2.
enum class UiItemKind : uint8_t {
    Flat,
    Sdf,
    Mesh,
    PathFill,
    PathClipPush,
    PathClipPop
};

struct UiItem {
    UiItemKind kind        = UiItemKind::Flat;
    uint16_t   textureIdx  = UINT16_MAX;  // Flat: white / font atlas / UI texture
    uint64_t   state       = 0;           // bgfx blend state (P1 encodes BlendMode)
    float      minX        = 0.0f;
    float      minY        = 0.0f;
    float      maxX        = 0.0f;
    float      maxY        = 0.0f;
    uint32_t   abgr[4]     = {0, 0, 0, 0};  // per-corner colors: TL TR BR BL
    float      u0          = 0.0f;          // Flat only
    float      v0          = 0.0f;
    float      u1          = 1.0f;
    float      v1          = 1.0f;
    detail::UiGpuContext::SdfParams sdf;    // P2: kind==Sdf only; quad bounds
    // Shape silhouette (Sdf only): the *content* rect. Differs from the
    // draw quad (minX..maxY, expanded by shadow/outside-stroke/blur).
    // Baked into vertices as (center, half-extent) at flush so SDF items
    // with identical sdf params can share one submission (run merging).
    float      shapeMinX    = 0.0f;
    float      shapeMinY    = 0.0f;
    float      shapeMaxX    = 0.0f;
    float      shapeMaxY    = 0.0f;
    // SDF only: the clip rect at record time (activeClipBounds — full
    // screen when no clip is pushed). Soft-clip seam AA, see UiVertex.
    float      clipMinX     = 0.0f;
    float      clipMinY     = 0.0f;
    float      clipMaxX     = 0.0f;
    float      clipMaxY     = 0.0f;
    // Vector path payload. Mesh items use `mesh`; fill/clip barriers use a
    // complete immutable snapshot so releasePath is safe before endFrame.
    std::shared_ptr<const UiTriangleMesh> mesh;
    std::shared_ptr<const UiPathSnapshot> path;
    uint8_t stencilDepth = 0;
};                                          //     (minX..maxY) already expanded

// SdfParams holds FVector4 members (16B-aligned f128) → trailing padding;
// memcmp would compare uninitialized tail bytes and never match. Compare
// the value fields only (the run-merge key for SDF batching).
bool sdfParamsEqual(const detail::UiGpuContext::SdfParams& a,
                    const detail::UiGpuContext::SdfParams& b)
{
    return a.radius == b.radius && a.strokeColor == b.strokeColor
        && a.strokeWidth == b.strokeWidth && a.strokeInset == b.strokeInset
        && a.shadowColor == b.shadowColor && a.shadowOffset == b.shadowOffset
        && a.shadowBlur == b.shadowBlur;
}

bool batchCompatible(const UiItem& a, const UiItem& b)
{
    if (a.kind != b.kind || a.state != b.state
        || a.stencilDepth != b.stencilDepth) {
        return false;
    }
    if (a.kind == UiItemKind::Flat) {
        return a.textureIdx == b.textureIdx;
    }
    if (a.kind == UiItemKind::Sdf) {
        return sdfParamsEqual(a.sdf, b.sdf);
    }
    return a.kind == UiItemKind::Mesh;
}

bool hasFiniteBounds(const UiItem& item)
{
    return std::isfinite(item.minX) && std::isfinite(item.minY)
        && std::isfinite(item.maxX) && std::isfinite(item.maxY);
}

bool drawBoundsOverlap(const UiItem& a, const UiItem& b)
{
    // Touching edges do not cover a common pixel. SDF draw bounds already
    // include AA, outside-stroke and shadow extents, so this test also guards
    // the visually significant falloff area.
    return a.minX < b.maxX && b.minX < a.maxX
        && a.minY < b.maxY && b.minY < a.maxY;
}

bool isOrderingBarrier(const UiItem& item)
{
    return item.kind == UiItemKind::PathFill
        || item.kind == UiItemKind::PathClipPush
        || item.kind == UiItemKind::PathClipPop;
}

#if !defined(NDEBUG)
bool validateOverlapAwareOrder(const std::vector<UiItem>& items,
                               const std::vector<uint32_t>& order,
                               std::vector<uint32_t>& positions)
{
    const size_t count = items.size();
    if (order.size() != count) {
        return false;
    }

    positions.assign(count, UINT32_MAX);
    for (uint32_t position = 0; position < static_cast<uint32_t>(count); ++position) {
        const uint32_t itemIndex = order[position];
        if (itemIndex >= count || positions[itemIndex] != UINT32_MAX) {
            return false;
        }
        positions[itemIndex] = position;
    }

    // The planner may reorder disjoint items, but every overlapping pair must
    // retain painter order. This independent O(N^2) check is Debug-only so
    // tests exercise the invariant without affecting the Release hot path.
    for (uint32_t earlier = 0; earlier < static_cast<uint32_t>(count); ++earlier) {
        for (uint32_t later = earlier + 1u;
             later < static_cast<uint32_t>(count); ++later) {
            if (drawBoundsOverlap(items[earlier], items[later])
                && positions[earlier] >= positions[later]) {
                return false;
            }
        }
    }
    return true;
}
#endif

// Material-aware, painter-order-safe scheduler. A later item may move before
// skipped items only when its draw bounds do not overlap any item it crosses.
// Therefore every overlapping pair retains its original relative order while
// disjoint controls (button/list/grid rows are the common case) can collapse
// into much larger batches. Work is bounded by a local look-ahead window;
// unlike an all-pairs overlap DAG this remains linear in N for a fixed window
// (O(N * window^2) worst case, with a deliberately small constant window).
bool buildOverlapAwareOrder(const std::vector<UiItem>& items,
                            std::vector<uint32_t>& order,
                            std::vector<uint32_t>& next,
                            std::vector<uint32_t>& barriers)
{
    constexpr uint32_t kInvalidIndex = UINT32_MAX;
    constexpr uint32_t kLookAhead    = 96u;

    const size_t count = items.size();
    order.clear();
    if (count < 3u || count > static_cast<size_t>(UINT32_MAX)) {
        return false;
    }

    for (const UiItem& item : items) {
        // Invalid geometry is treated as a planner failure, not as disjoint
        // geometry. The caller then uses the original ordered-run fallback.
        if (!hasFiniteBounds(item)) {
            return false;
        }
    }

    next.resize(count);
    order.reserve(count);
    barriers.reserve(kLookAhead);

    // Path fill/clip entries mutate stencil state and are hard ordering
    // barriers. Optimize each ordinary interval independently so a single
    // vector path does not disable batching for the rest of the frame.
    const auto appendOptimizedSegment = [&](uint32_t begin, uint32_t end) {
        if (begin >= end) return;
        if (end - begin < 3u) {
            for (uint32_t i = begin; i < end; ++i) order.push_back(i);
            return;
        }

        for (uint32_t i = begin; i < end; ++i) {
            next[i] = (i + 1u < end) ? i + 1u : kInvalidIndex;
        }

        uint32_t head = begin;
        while (head != kInvalidIndex) {
            const uint32_t anchor = head;
            order.push_back(anchor);

            barriers.clear();
            uint32_t previous  = anchor;
            uint32_t candidate = next[anchor];
            uint32_t inspected = 0u;

            while (candidate != kInvalidIndex && inspected < kLookAhead) {
                const uint32_t afterCandidate = next[candidate];
                bool canMove = batchCompatible(items[anchor], items[candidate]);
                if (canMove) {
                    for (uint32_t barrier : barriers) {
                        if (drawBoundsOverlap(items[candidate], items[barrier])) {
                            canMove = false;
                            break;
                        }
                    }
                }

                if (canMove) {
                    // Remove candidate from the remaining linked list and append
                    // it next to the anchor in the optimized logical order.
                    order.push_back(candidate);
                    next[previous] = afterCandidate;
                } else {
                    barriers.push_back(candidate);
                    previous = candidate;
                }

                candidate = afterCandidate;
                ++inspected;
            }

            head = next[anchor];
        }
    };

    uint32_t segmentBegin = 0u;
    for (uint32_t i = 0u; i < static_cast<uint32_t>(count); ++i) {
        if (!isOrderingBarrier(items[i])) continue;
        appendOptimizedSegment(segmentBegin, i);
        order.push_back(i);
        segmentBegin = i + 1u;
    }
    appendOptimizedSegment(segmentBegin, static_cast<uint32_t>(count));

    if (order.size() != count) {
        order.clear();
        return false;
    }
#if !defined(NDEBUG)
    if (!validateOverlapAwareOrder(items, order, next)) {
        order.clear();
        return false;
    }
#endif
    return true;
}

// Per-corner radii helpers. Shader u_radius order is TL TR BR BL —
// CornerRadii field order matches 1:1. Each corner clamps to maxRadius.
float clampRadius(float r, float maxRadius)
{
    return std::max(0.0f, std::min(r, maxRadius));
}

ayt::math::FVector4 radiiVec4(const ayt::ui::IRenderBackend::CornerRadii& radii, float maxRadius)
{
    return ayt::math::FVector4(clampRadius(radii.topLeft, maxRadius),
                               clampRadius(radii.topRight, maxRadius),
                               clampRadius(radii.bottomRight, maxRadius),
                               clampRadius(radii.bottomLeft, maxRadius));
}

// BorderStyle::Position → SdfParams.strokeInset (ring center offset):
// Outside = +w/2 (band d ∈ [0, w], flush outside the rect edge),
// Center = 0 (straddles the edge), Inside = -w/2 (band d ∈ [-w, 0]).
float borderInsetForPosition(ayt::ui::IRenderBackend::BorderStyle::Position position,
                             float strokeWidth)
{
    switch (position) {
    case ayt::ui::IRenderBackend::BorderStyle::Position::Outside:
        return strokeWidth * 0.5f;
    case ayt::ui::IRenderBackend::BorderStyle::Position::Inside:
        return -strokeWidth * 0.5f;
    case ayt::ui::IRenderBackend::BorderStyle::Position::Center:
    default:
        return 0.0f;
    }
}

uint32_t toAbgr(const ayt::math::FVector4& color)
{
    const uint8_t r = static_cast<uint8_t>(color.x * 255.0f);
    const uint8_t g = static_cast<uint8_t>(color.y * 255.0f);
    const uint8_t b = static_cast<uint8_t>(color.z * 255.0f);
    const uint8_t a = static_cast<uint8_t>(color.w * 255.0f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

uint32_t toRgbaClear(const ayt::math::FVector4& color)
{
    const auto channel = [](float value) {
        return static_cast<uint8_t>(std::lround(
            std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const uint32_t r = channel(color.x);
    const uint32_t g = channel(color.y);
    const uint32_t b = channel(color.z);
    const uint32_t a = channel(color.w);
    return (r << 24u) | (g << 16u) | (b << 8u) | a;
}

ayt::math::FVector4 premultipliedLayerColor(const ayt::math::FVector4& color)
{
    const float alpha = std::clamp(color.w, 0.0f, 1.0f);
    return ayt::math::FVector4(
        std::clamp(color.x, 0.0f, 1.0f) * alpha,
        std::clamp(color.y, 0.0f, 1.0f) * alpha,
        std::clamp(color.z, 0.0f, 1.0f) * alpha,
        alpha);
}

float toNdcX(float px, float width)
{
    return (px / width) * 2.0f - 1.0f;
}

float toNdcY(float py, float height)
{
    return 1.0f - (py / height) * 2.0f;
}

ayt::math::FRectangle intersectRect(const ayt::math::FRectangle& a,
                                    const ayt::math::FRectangle& b)
{
    return ayt::math::FRectangle(
        std::max(a.minX, b.minX),
        std::max(a.minY, b.minY),
        std::min(a.maxX, b.maxX),
        std::min(a.maxY, b.maxY));
}

bool rectNonEmpty(const ayt::math::FRectangle& r)
{
    return r.maxX > r.minX && r.maxY > r.minY;
}

ayt::math::FRectangle pixelAlignedLayerBounds(
    const ayt::math::FRectangle& logicalBounds, float dpiScale)
{
    // A Layer whose logical origin/extent is fractional cannot map directly
    // to ceil(width*dpi) pixels: using the unsnapped origin shifts offscreen
    // pixel centres relative to the main framebuffer, and a later point-sampled
    // composite produces one-pixel seams. Retain the full outward-snapped
    // physical coverage and clip the composite back to logicalBounds.
    return ayt::math::FRectangle(
        std::floor(logicalBounds.minX * dpiScale) / dpiScale,
        std::floor(logicalBounds.minY * dpiScale) / dpiScale,
        std::ceil(logicalBounds.maxX * dpiScale) / dpiScale,
        std::ceil(logicalBounds.maxY * dpiScale) / dpiScale);
}

constexpr float kPathEpsilon = 1.0e-5f;
constexpr float kPi = 3.14159265358979323846f;

float pathCross(const ayt::math::FVector2& a,
                const ayt::math::FVector2& b,
                const ayt::math::FVector2& c)
{
    return (b.x - a.x) * (c.y - a.y)
         - (b.y - a.y) * (c.x - a.x);
}

float pathLength(float x, float y)
{
    return std::sqrt(x * x + y * y);
}

std::vector<ayt::math::FVector2> sanitizeContour(
    const std::vector<ayt::math::FVector2>& input)
{
    std::vector<ayt::math::FVector2> output;
    output.reserve(input.size());
    for (const auto& point : input) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) continue;
        if (!output.empty()) {
            const float dx = point.x - output.back().x;
            const float dy = point.y - output.back().y;
            if (dx * dx + dy * dy <= kPathEpsilon * kPathEpsilon) continue;
        }
        output.push_back(point);
    }
    if (output.size() > 1u) {
        const float dx = output.front().x - output.back().x;
        const float dy = output.front().y - output.back().y;
        if (dx * dx + dy * dy <= kPathEpsilon * kPathEpsilon) {
            output.pop_back();
        }
    }
    return output;
}

bool pointInsideTriangle(const ayt::math::FVector2& p,
                         const ayt::math::FVector2& a,
                         const ayt::math::FVector2& b,
                         const ayt::math::FVector2& c,
                         float orientation)
{
    const float ab = pathCross(a, b, p) * orientation;
    const float bc = pathCross(b, c, p) * orientation;
    const float ca = pathCross(c, a, p) * orientation;
    return ab >= -kPathEpsilon && bc >= -kPathEpsilon && ca >= -kPathEpsilon;
}

UiTriangleMesh triangulateContour(const std::vector<ayt::math::FVector2>& raw)
{
    UiTriangleMesh mesh;
    mesh.positions = sanitizeContour(raw);
    const size_t count = mesh.positions.size();
    if (count < 3u) {
        mesh.positions.clear();
        return mesh;
    }

    float twiceArea = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const auto& a = mesh.positions[i];
        const auto& b = mesh.positions[(i + 1u) % count];
        twiceArea += a.x * b.y - b.x * a.y;
    }
    if (std::fabs(twiceArea) <= kPathEpsilon) {
        mesh.positions.clear();
        return mesh;
    }
    const float orientation = twiceArea > 0.0f ? 1.0f : -1.0f;

    std::vector<uint32_t> polygon(count);
    for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) polygon[i] = i;
    mesh.indices.reserve((count - 2u) * 3u);

    size_t guard = count * count;
    while (polygon.size() > 3u && guard-- > 0u) {
        bool clippedEar = false;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const uint32_t prev = polygon[(i + polygon.size() - 1u) % polygon.size()];
            const uint32_t curr = polygon[i];
            const uint32_t next = polygon[(i + 1u) % polygon.size()];
            const auto& a = mesh.positions[prev];
            const auto& b = mesh.positions[curr];
            const auto& c = mesh.positions[next];
            if (pathCross(a, b, c) * orientation <= kPathEpsilon) continue;

            bool containsPoint = false;
            for (uint32_t candidate : polygon) {
                if (candidate == prev || candidate == curr || candidate == next) continue;
                if (pointInsideTriangle(mesh.positions[candidate], a, b, c, orientation)) {
                    containsPoint = true;
                    break;
                }
            }
            if (containsPoint) continue;

            mesh.indices.push_back(prev);
            mesh.indices.push_back(curr);
            mesh.indices.push_back(next);
            polygon.erase(polygon.begin() + static_cast<std::ptrdiff_t>(i));
            clippedEar = true;
            break;
        }
        if (!clippedEar) break;
    }
    if (polygon.size() == 3u) {
        mesh.indices.push_back(polygon[0]);
        mesh.indices.push_back(polygon[1]);
        mesh.indices.push_back(polygon[2]);
    }
    if (mesh.indices.size() != (count - 2u) * 3u) {
        mesh.positions.clear();
        mesh.indices.clear();
    }
    return mesh;
}

template <typename Inside, typename Intersect>
std::vector<ayt::math::FVector2> clipPolygonEdge(
    const std::vector<ayt::math::FVector2>& input,
    Inside inside, Intersect intersect)
{
    std::vector<ayt::math::FVector2> output;
    if (input.empty()) return output;
    output.reserve(input.size() + 2u);
    ayt::math::FVector2 previous = input.back();
    bool previousInside = inside(previous);
    for (const auto& current : input) {
        const bool currentInside = inside(current);
        if (currentInside != previousInside) {
            output.push_back(intersect(previous, current));
        }
        if (currentInside) output.push_back(current);
        previous = current;
        previousInside = currentInside;
    }
    return output;
}

UiTriangleMesh clipMeshToRect(const UiTriangleMesh& input,
                              const ayt::math::FRectangle& clip)
{
    UiTriangleMesh output;
    if (!rectNonEmpty(clip)) return output;
    for (size_t tri = 0; tri + 2u < input.indices.size(); tri += 3u) {
        std::vector<ayt::math::FVector2> polygon = {
            input.positions[input.indices[tri]],
            input.positions[input.indices[tri + 1u]],
            input.positions[input.indices[tri + 2u]]
        };
        const auto verticalIntersect = [](float x) {
            return [x](const ayt::math::FVector2& a,
                       const ayt::math::FVector2& b) {
                const float dx = b.x - a.x;
                const float t = std::fabs(dx) > kPathEpsilon ? (x - a.x) / dx : 0.0f;
                return ayt::math::FVector2(x, a.y + (b.y - a.y) * t);
            };
        };
        const auto horizontalIntersect = [](float y) {
            return [y](const ayt::math::FVector2& a,
                       const ayt::math::FVector2& b) {
                const float dy = b.y - a.y;
                const float t = std::fabs(dy) > kPathEpsilon ? (y - a.y) / dy : 0.0f;
                return ayt::math::FVector2(a.x + (b.x - a.x) * t, y);
            };
        };
        polygon = clipPolygonEdge(polygon,
            [&](const auto& p) { return p.x >= clip.minX; }, verticalIntersect(clip.minX));
        polygon = clipPolygonEdge(polygon,
            [&](const auto& p) { return p.x <= clip.maxX; }, verticalIntersect(clip.maxX));
        polygon = clipPolygonEdge(polygon,
            [&](const auto& p) { return p.y >= clip.minY; }, horizontalIntersect(clip.minY));
        polygon = clipPolygonEdge(polygon,
            [&](const auto& p) { return p.y <= clip.maxY; }, horizontalIntersect(clip.maxY));
        if (polygon.size() < 3u) continue;
        const uint32_t base = static_cast<uint32_t>(output.positions.size());
        output.positions.insert(output.positions.end(), polygon.begin(), polygon.end());
        for (uint32_t i = 1u; i + 1u < static_cast<uint32_t>(polygon.size()); ++i) {
            output.indices.push_back(base);
            output.indices.push_back(base + i);
            output.indices.push_back(base + i + 1u);
        }
    }
    return output;
}

void appendMesh(UiTriangleMesh& destination, const UiTriangleMesh& source)
{
    const uint32_t base = static_cast<uint32_t>(destination.positions.size());
    destination.positions.insert(destination.positions.end(),
                                 source.positions.begin(), source.positions.end());
    destination.indices.reserve(destination.indices.size() + source.indices.size());
    for (uint32_t index : source.indices) destination.indices.push_back(base + index);
}

void appendStrokeTriangle(UiTriangleMesh& mesh,
                          const ayt::math::FVector2& a,
                          const ayt::math::FVector2& b,
                          const ayt::math::FVector2& c)
{
    const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
    mesh.positions.insert(mesh.positions.end(), {a, b, c});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1u, base + 2u});
}

void appendStrokeQuad(UiTriangleMesh& mesh,
                      const ayt::math::FVector2& a,
                      const ayt::math::FVector2& b,
                      const ayt::math::FVector2& c,
                      const ayt::math::FVector2& d)
{
    const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
    mesh.positions.insert(mesh.positions.end(), {a, b, c, d});
    mesh.indices.insert(mesh.indices.end(), {
        base, base + 1u, base + 2u,
        base, base + 2u, base + 3u
    });
}

void appendStrokeSector(UiTriangleMesh& mesh,
                        const ayt::math::FVector2& center,
                        float radius, float startAngle, float sweep)
{
    if (radius <= kPathEpsilon || std::fabs(sweep) <= kPathEpsilon) return;
    const float density = std::max(2.0f, std::sqrt(std::max(radius, 0.5f) * 8.0f));
    const int segments = std::clamp(
        static_cast<int>(std::ceil(std::fabs(sweep) * density)), 1, 64);
    const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
    mesh.positions.push_back(center);
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = startAngle + sweep * t;
        mesh.positions.emplace_back(center.x + std::cos(angle) * radius,
                                    center.y + std::sin(angle) * radius);
    }
    for (int i = 0; i < segments; ++i) {
        mesh.indices.insert(mesh.indices.end(), {
            base, base + 1u + static_cast<uint32_t>(i),
            base + 2u + static_cast<uint32_t>(i)
        });
    }
}

UiTriangleMesh tessellateStroke(const std::vector<UiPathContour>& contours,
                                float width, ayt::ui::PathStrokeCap cap,
                                ayt::ui::PathStrokeJoin join, float miterLimit)
{
    UiTriangleMesh output;
    const float halfWidth = std::max(0.0f, width) * 0.5f;
    if (halfWidth <= 0.0f) return output;
    miterLimit = std::max(1.0f, miterLimit);

    // Preserve the established compact strip for the legacy default.  The
    // contour-aware tessellator below is selected when authored SVG stroke
    // semantics require round/bevel joins or non-butt caps.
    if (cap == ayt::ui::PathStrokeCap::Butt
        && join == ayt::ui::PathStrokeJoin::Miter) {
        for (const UiPathContour& contour : contours) {
            const auto points = sanitizeContour(contour.points);
            const size_t count = points.size();
            if (count < 2u) continue;
            const bool closed = contour.closed && count > 2u;
            const size_t segmentCount = closed ? count : count - 1u;
            const uint32_t base = static_cast<uint32_t>(output.positions.size());
            output.positions.reserve(output.positions.size() + count * 2u);
            for (size_t i = 0; i < count; ++i) {
                const size_t previous = i == 0u ? (closed ? count - 1u : 0u) : i - 1u;
                const size_t next = i + 1u < count ? i + 1u : (closed ? 0u : count - 1u);
                float previousDx = points[i].x - points[previous].x;
                float previousDy = points[i].y - points[previous].y;
                float nextDx = points[next].x - points[i].x;
                float nextDy = points[next].y - points[i].y;
                if (!closed && i == 0u) { previousDx = nextDx; previousDy = nextDy; }
                if (!closed && i + 1u == count) { nextDx = previousDx; nextDy = previousDy; }
                const float previousLength = std::max(
                    pathLength(previousDx, previousDy), kPathEpsilon);
                const float nextLength = std::max(pathLength(nextDx, nextDy), kPathEpsilon);
                const float previousNx = -previousDy / previousLength;
                const float previousNy = previousDx / previousLength;
                const float nextNx = -nextDy / nextLength;
                const float nextNy = nextDx / nextLength;
                float miterX = previousNx + nextNx;
                float miterY = previousNy + nextNy;
                const float miterLength = pathLength(miterX, miterY);
                if (miterLength <= kPathEpsilon) {
                    miterX = nextNx;
                    miterY = nextNy;
                } else {
                    miterX /= miterLength;
                    miterY /= miterLength;
                }
                const float denominator = std::max(0.25f,
                    std::fabs(miterX * nextNx + miterY * nextNy));
                const float extent = std::min(halfWidth / denominator,
                                              halfWidth * miterLimit);
                output.positions.emplace_back(points[i].x + miterX * extent,
                                              points[i].y + miterY * extent);
                output.positions.emplace_back(points[i].x - miterX * extent,
                                              points[i].y - miterY * extent);
            }
            for (size_t i = 0; i < segmentCount; ++i) {
                const size_t next = (i + 1u) % count;
                const uint32_t a = base + static_cast<uint32_t>(i * 2u);
                const uint32_t b = base + static_cast<uint32_t>(next * 2u);
                output.indices.insert(output.indices.end(), {
                    a, b, b + 1u, a, b + 1u, a + 1u
                });
            }
        }
        return output;
    }

    for (const UiPathContour& contour : contours) {
        const auto points = sanitizeContour(contour.points);
        const size_t count = points.size();
        if (count < 2u) continue;
        const bool closed = contour.closed && count > 2u;
        const size_t segmentCount = closed ? count : count - 1u;
        std::vector<ayt::math::FVector2> directions(segmentCount);
        std::vector<ayt::math::FVector2> normals(segmentCount);
        for (size_t i = 0; i < segmentCount; ++i) {
            const size_t next = (i + 1u) % count;
            float dx = points[next].x - points[i].x;
            float dy = points[next].y - points[i].y;
            const float length = std::max(pathLength(dx, dy), kPathEpsilon);
            dx /= length;
            dy /= length;
            directions[i] = {dx, dy};
            normals[i] = {-dy, dx};

            ayt::math::FVector2 start = points[i];
            ayt::math::FVector2 end = points[next];
            if (!closed && cap == ayt::ui::PathStrokeCap::Square) {
                if (i == 0u) {
                    start.x -= dx * halfWidth;
                    start.y -= dy * halfWidth;
                }
                if (i + 1u == segmentCount) {
                    end.x += dx * halfWidth;
                    end.y += dy * halfWidth;
                }
            }
            const ayt::math::FVector2 offset(normals[i].x * halfWidth,
                                             normals[i].y * halfWidth);
            appendStrokeQuad(output,
                {start.x + offset.x, start.y + offset.y},
                {end.x + offset.x, end.y + offset.y},
                {end.x - offset.x, end.y - offset.y},
                {start.x - offset.x, start.y - offset.y});
        }

        const size_t joinCount = closed ? count : count - 2u;
        for (size_t joinOffset = 0; joinOffset < joinCount; ++joinOffset) {
            const size_t pointIndex = closed ? joinOffset : joinOffset + 1u;
            const size_t previousSegment = (pointIndex + segmentCount - 1u) % segmentCount;
            const size_t nextSegment = pointIndex % segmentCount;
            const auto& d0 = directions[previousSegment];
            const auto& d1 = directions[nextSegment];
            const auto& n0 = normals[previousSegment];
            const auto& n1 = normals[nextSegment];
            const float turn = d0.x * d1.y - d0.y * d1.x;
            const float alignment = d0.x * d1.x + d0.y * d1.y;
            if (std::fabs(turn) <= kPathEpsilon && alignment > 0.0f) continue;

            const auto& center = points[pointIndex];
            if (std::fabs(turn) <= kPathEpsilon) {
                if (join == ayt::ui::PathStrokeJoin::Round) {
                    appendStrokeSector(output, center, halfWidth, 0.0f, kPi * 2.0f);
                }
                continue;
            }

            const float outerSign = turn > 0.0f ? -1.0f : 1.0f;
            const ayt::math::FVector2 outer0(n0.x * outerSign, n0.y * outerSign);
            const ayt::math::FVector2 outer1(n1.x * outerSign, n1.y * outerSign);
            const ayt::math::FVector2 inner0(-outer0.x, -outer0.y);
            const ayt::math::FVector2 inner1(-outer1.x, -outer1.y);
            const ayt::math::FVector2 outerPoint0(
                center.x + outer0.x * halfWidth,
                center.y + outer0.y * halfWidth);
            const ayt::math::FVector2 outerPoint1(
                center.x + outer1.x * halfWidth,
                center.y + outer1.y * halfWidth);

            appendStrokeTriangle(output, center,
                {center.x + inner0.x * halfWidth,
                 center.y + inner0.y * halfWidth},
                {center.x + inner1.x * halfWidth,
                 center.y + inner1.y * halfWidth});

            if (join == ayt::ui::PathStrokeJoin::Round) {
                const float start = std::atan2(outer0.y, outer0.x);
                float sweep = std::atan2(
                    outer0.x * outer1.y - outer0.y * outer1.x,
                    outer0.x * outer1.x + outer0.y * outer1.y);
                if (turn > 0.0f && sweep < 0.0f) sweep += kPi * 2.0f;
                if (turn < 0.0f && sweep > 0.0f) sweep -= kPi * 2.0f;
                appendStrokeSector(output, center, halfWidth, start, sweep);
                continue;
            }

            appendStrokeTriangle(output, center, outerPoint0, outerPoint1);
            if (join == ayt::ui::PathStrokeJoin::Miter) {
                float bisectorX = outer0.x + outer1.x;
                float bisectorY = outer0.y + outer1.y;
                const float bisectorLength = pathLength(bisectorX, bisectorY);
                if (bisectorLength > kPathEpsilon) {
                    bisectorX /= bisectorLength;
                    bisectorY /= bisectorLength;
                    const float denominator = std::fabs(
                        bisectorX * outer1.x + bisectorY * outer1.y);
                    if (denominator > kPathEpsilon) {
                        const float extent = halfWidth / denominator;
                        if (extent <= halfWidth * miterLimit) {
                            appendStrokeTriangle(output, outerPoint0,
                                {center.x + bisectorX * extent,
                                 center.y + bisectorY * extent},
                                outerPoint1);
                        }
                    }
                }
            }
        }

        if (!closed && cap == ayt::ui::PathStrokeCap::Round) {
            const auto& firstNormal = normals.front();
            appendStrokeSector(output, points.front(), halfWidth,
                std::atan2(firstNormal.y, firstNormal.x), kPi);
            const auto& lastNormal = normals.back();
            appendStrokeSector(output, points.back(), halfWidth,
                std::atan2(-lastNormal.y, -lastNormal.x), kPi);
        }
    }
    return output;
}

void includeMeshBounds(UiPathSnapshot& snapshot, const UiTriangleMesh& mesh)
{
    for (const auto& point : mesh.positions) {
        if (!snapshot.hasBounds) {
            snapshot.bounds = ayt::math::FRectangle(point.x, point.y, point.x, point.y);
            snapshot.hasBounds = true;
        } else {
            snapshot.bounds.minX = std::min(snapshot.bounds.minX, point.x);
            snapshot.bounds.minY = std::min(snapshot.bounds.minY, point.y);
            snapshot.bounds.maxX = std::max(snapshot.bounds.maxX, point.x);
            snapshot.bounds.maxY = std::max(snapshot.bounds.maxY, point.y);
        }
    }
}

std::shared_ptr<UiPathSnapshot> snapshotPath(
    const UiPathData& path, const ayt::math::FRectangle& clip)
{
    auto snapshot = std::make_shared<UiPathSnapshot>();
    for (const UiPathContour& contour : path.contours) {
        if (!contour.closed) continue;
        UiTriangleMesh mesh = clipMeshToRect(triangulateContour(contour.points), clip);
        if (mesh.indices.empty()) continue;
        includeMeshBounds(*snapshot, mesh);
        snapshot->fillContours.push_back({std::move(mesh), contour.winding});
    }
    snapshot->strokeMesh = clipMeshToRect(
        tessellateStroke(path.contours, path.strokeWidth, path.strokeCap,
                         path.strokeJoin, path.miterLimit), clip);
    includeMeshBounds(*snapshot, snapshot->strokeMesh);
    return snapshot;
}

int adaptiveArcSegments(float radius, float sweep)
{
    const float r = std::max(1.0f, std::fabs(radius));
    const float maxStep = std::acos(std::max(-1.0f, 1.0f - 0.25f / r));
    const int segments = static_cast<int>(std::ceil(
        std::fabs(sweep) / std::max(maxStep, kPi / 64.0f)));
    return std::clamp(segments, 4, 128);
}

std::vector<ayt::math::FVector2> sampleArc(
    const ayt::math::FVector2& center, float radiusX, float radiusY,
    float startAngle, float endAngle, bool includeEnd)
{
    const float sweep = endAngle - startAngle;
    const int segments = adaptiveArcSegments(std::max(radiusX, radiusY), sweep);
    const int pointCount = includeEnd ? segments + 1 : segments;
    std::vector<ayt::math::FVector2> points;
    points.reserve(static_cast<size_t>(pointCount));
    for (int i = 0; i < pointCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = startAngle + sweep * t;
        points.emplace_back(center.x + std::cos(angle) * radiusX,
                            center.y + std::sin(angle) * radiusY);
    }
    return points;
}

// P1: BlendMode -> bgfx blend state. Write bits are folded in here so
// every recorded item state is self-sufficient (submit() skips setState
// when the passed state is 0, so state must never be 0). Additive uses
// FUNC(SRC_ALPHA, ONE) to match the interface contract SRC*SRC_ALPHA+DST*1
// (plain BGFX_STATE_BLEND_ADD would be ONE,ONE = SRC*1+DST*1). Every
// non-Normal mode still accumulates alpha as source-over coverage; bgfx's
// convenience Multiply/Screen macros reuse their RGB factors for alpha and
// would make an opaque isolated Layer translucent after one blended draw.
ayt::font::ShapingDirection toFontDirection(ayt::ui::TextDirection direction)
{
    switch (direction) {
    case ayt::ui::TextDirection::LeftToRight:
        return ayt::font::ShapingDirection::LeftToRight;
    case ayt::ui::TextDirection::RightToLeft:
        return ayt::font::ShapingDirection::RightToLeft;
    case ayt::ui::TextDirection::Auto:
    default:
        return ayt::font::ShapingDirection::Auto;
    }
}

uint64_t blendStateBits(ayt::ui::BlendMode mode)
{
    uint64_t bits = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    switch (mode) {
    case ayt::ui::BlendMode::Additive:
        bits |= BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE,
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        break;
    case ayt::ui::BlendMode::Multiply:
        bits |= BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO,
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        break;
    case ayt::ui::BlendMode::Screen:
        bits |= BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_COLOR,
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        break;
    case ayt::ui::BlendMode::Normal:
    default:
        // Straight-alpha source-over. RGB matches BGFX_STATE_BLEND_ALPHA,
        // but alpha must accumulate coverage as As + Ad*(1-As), not
        // As*As + Ad*(1-As). The distinction is essential when the result
        // is retained in a transparent Layer and sampled again later.
        bits |= BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
        break;
    }
    return bits;
}

uint64_t premultipliedOverStateBits()
{
    return BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
        | BGFX_STATE_BLEND_FUNC_SEPARATE(
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA,
            BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
}

// P3: one live UI texture in the registry. textureIdx is the bgfx handle
// (uploaded via uploadUiTexture — linear + clamp), width/height back the
// 9-patch UV math (padding is in texture pixels, so corners map 1:1 to
// screen pixels). Handles are fake pointers from an increasing counter —
// never reused, so a stale handle can never alias a new texture.
struct TextureRef {
    uint16_t textureIdx = detail::UiGpuContext::kInvalidIdx;
    uint32_t refCount   = 0;
    uint16_t width      = 0;
    uint16_t height     = 0;
};

struct RenderTargetRef {
    ayt::ui::IRenderBackend::RenderTargetDesc desc{};
    detail::PooledRenderTargetHandle pooled{};
    void* textureToken = nullptr;
};

struct LayerRef {
    ayt::ui::IRenderBackend::LayerDesc desc{};
    ayt::math::FRectangle backingBounds{};
    ayt::ui::IRenderBackend::RenderTargetHandle target{};
    ayt::math::FRectangle damage{};
    bool dirty = true;
    bool painting = false;
    uint64_t lastCompositeFrame = 0;
    uint64_t lastPaintFrame = 0;
};

// Pre-P3 gray-block fallback for unknown / released handles.
const ayt::math::FVector4 kUnknownTextureColor(0.25f, 0.25f, 0.28f, 1.0f);

} // namespace

namespace {

struct UiClipEntry {
    ayt::math::FRectangle bounds;
    bool pathClip = false;
    std::shared_ptr<const UiPathSnapshot> path;
    uint8_t depth = 0;
};

} // namespace

struct UIRenderBackend::FrameState {
    std::vector<UiItem>                items;              // ordered draw list (z-order)
    std::vector<UiVertex>              scratchVertices;
    std::vector<uint32_t>              scratchIndices;
    std::vector<UiClipEntry>           clipStack;
    uint8_t                            pathClipDepth = 0;
    bool                               hasOrderingBarriers = false;
    ayt::ui::BlendMode                 currentBlend = ayt::ui::BlendMode::Normal;
    BatchMode                          batchMode = BatchMode::OverlapAware;
    float                              uiScale = 1.0f;
    struct TransformState {
        bool applied = false;
        float uiScale = 1.0f;
        float originX = 0.0f;
        float originY = 0.0f;
        std::vector<UiClipEntry> clipStack;
    };
    std::vector<TransformState>        transformStack;
    // Overlap-aware planner scratch. These vectors retain capacity across
    // frames; beginFrame clears only the logical command stream.
    std::vector<uint32_t>              batchOrder;
    std::vector<uint32_t>              batchNext;
    std::vector<uint32_t>              batchBarriers;
    // PR-anim: stacked global opacity (LIFO, mirrors clipStack). Every
    // color-emitting entry multiplies its alpha by the stack top; the
    // base frame is 1.0 so default rendering is byte-identical.
    std::vector<float> opacityStack = {1.0f};
    // P3: texture registry — persistent across frames (beginFrame does not
    // touch it; handles stay valid until releaseUiTexture frees them).
    std::unordered_map<void*, TextureRef> textures;
    uintptr_t                             nextHandle = 1;  // fake-pointer counter
    std::unordered_map<int, RenderTargetRef> renderTargets;
    std::unordered_map<void*, int> targetTextures;
    std::unordered_map<int, LayerRef> layers;
    int nextTargetId = 1;
    int nextLayerId = 1;
    RenderTargetHandle boundTarget{};
    LayerHandle activeLayer{};
    uint16_t activeWidth = 0;
    uint16_t activeHeight = 0;
    uint8_t activeViewId = kViewId;
    uint16_t nextOffscreenViewId = kFirstLayerViewId;
    Renderer* renderer = nullptr;
    uint64_t frameIndex = 0;
    ayt::ui::IRenderBackend::LayerCacheStats layerStats{};
    float originX = 0.0f;
    float originY = 0.0f;
    // A target transition is a batch barrier. The caller-visible render
    // state is restored after the offscreen pass, even if a subtree leaks a
    // clip/opacity push internally.
    std::vector<UiClipEntry> savedClipStack;
    std::vector<float> savedOpacityStack;
    uint8_t savedPathClipDepth = 0;
    ayt::ui::BlendMode savedBlend = ayt::ui::BlendMode::Normal;
    float savedUiScale = 1.0f;
    // Path resources are CPU-side and persistent across frames. Draw/clip
    // commands hold immutable snapshots, so releasePath may run immediately
    // after recording without invalidating the pending frame.
    std::unordered_map<int, UiPathData> paths;
    int nextPathId = 1;
};

namespace {

// Takes the registry map (not FrameState — that type is private to
// UIRenderBackend) so free functions can look up handles.
const TextureRef* findTextureRef(const std::unordered_map<void*, TextureRef>& textures,
                                 void* handle)
{
    if (handle == nullptr) {
        return nullptr;
    }
    auto it = textures.find(handle);
    return (it == textures.end()) ? nullptr : &it->second;
}

} // namespace

UIRenderBackend::UIRenderBackend()
    : _frame(std::make_unique<FrameState>())
{
}

UIRenderBackend::~UIRenderBackend()
{
    shutdown();
}

bool UIRenderBackend::initialize(Renderer& renderer)
{
    detail::BGFXAdapter* adapter = renderer.bgfxAdapter();
    shader::ShaderResourcePool* pool = renderer.shaderPool();
    if (adapter == nullptr || pool == nullptr) {
        return false;
    }
    return initializeFromRenderer(renderer, *adapter, *pool);
}

bool UIRenderBackend::initializeFromRenderer(Renderer& renderer, detail::BGFXAdapter& adapter,
                                             shader::ShaderResourcePool& shaderPool)
{
    if (_initialized) {
        return true;
    }

    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    _gpu        = std::make_unique<detail::UiGpuContext>();
    _adapter    = &adapter;
    _targetPool = renderer.renderTargetPool();
    _frame->renderer = &renderer;
    _shaderPool = &shaderPool;
    if (!_gpu->initialize(shaderPool, adapter)) {
        shutdownFromRenderer(adapter, shaderPool);
        return false;
    }

    _fontAtlas = std::make_unique<detail::BgfxFontAtlas>();
    if (!_fontAtlas->initialize(adapter)) {
        std::fprintf(stderr, "[UIRenderBackend] BgfxFontAtlas initialize failed\n");
        shutdownFromRenderer(adapter, shaderPool);
        return false;
    }

    _initialized = true;
    return true;
}

void UIRenderBackend::shutdown()
{
    if (!_initialized) {
        return;
    }
    if (_adapter != nullptr && _shaderPool != nullptr) {
        shutdownFromRenderer(*_adapter, *_shaderPool);
    } else {
        shutdownFromRendererWithoutAdapter();
    }
}

void UIRenderBackend::shutdownFromRenderer(detail::BGFXAdapter& adapter,
                                           shader::ShaderResourcePool& shaderPool)
{
    if (_frame != nullptr) {
        // P3: release every live UI texture before the GPU context dies.
        // nextHandle intentionally NOT reset — fake pointers never reused,
        // so handles handed out before shutdown stay dead after reinit.
        if (_gpu != nullptr) {
            for (const auto& entry : _frame->textures) {
                _gpu->releaseTextTexture(adapter, entry.second.textureIdx);
            }
        }
        _frame->textures.clear();
        if (_targetPool != nullptr) {
            for (const auto& entry : _frame->renderTargets) {
                _targetPool->release(entry.second.pooled);
            }
        }
        _frame->renderTargets.clear();
        _frame->targetTextures.clear();
        _frame->layers.clear();
        _frame->paths.clear();
        _frame->items.clear();
        _frame->scratchVertices.clear();
        _frame->scratchIndices.clear();
    }

    if (_fontAtlas != nullptr) {
        _fontAtlas->shutdown(adapter);
        _fontAtlas.reset();
    }

    if (_gpu != nullptr) {
        _gpu->shutdown(shaderPool, adapter);
        _gpu.reset();
    }

    _adapter     = nullptr;
    _targetPool  = nullptr;
    _shaderPool  = nullptr;
    _initialized = false;
}

void UIRenderBackend::shutdownFromRendererWithoutAdapter()
{
    if (_frame != nullptr) {
        // No adapter to destroy through; the bgfx context is gone with the
        // renderer anyway. Drop the registry without touching GPU objects.
        _frame->textures.clear();
        _frame->renderTargets.clear();
        _frame->targetTextures.clear();
        _frame->layers.clear();
        _frame->paths.clear();
        _frame->items.clear();
        _frame->scratchVertices.clear();
        _frame->scratchIndices.clear();
    }

    _gpu.reset();
    _fontAtlas.reset();
    _adapter     = nullptr;
    _targetPool  = nullptr;
    _shaderPool  = nullptr;
    _initialized = false;
}

void UIRenderBackend::setFramebufferSize(uint16_t width, uint16_t height)
{
    _width  = width;
    _height = height;
}

bool UIRenderBackend::supportsRenderTargets() const
{
    return _initialized && _adapter != nullptr && _adapter->isInitialized()
        && _targetPool != nullptr;
}

UIRenderBackend::RenderTargetHandle UIRenderBackend::createRenderTarget(
    int width, int height, bool hasAlpha)
{
    RenderTargetDesc desc;
    desc.width = width;
    desc.height = height;
    desc.hasAlpha = hasAlpha;
    return createRenderTarget(desc);
}

UIRenderBackend::RenderTargetHandle UIRenderBackend::createRenderTarget(
    const RenderTargetDesc& desc)
{
    if (!supportsRenderTargets() || desc.width <= 0 || desc.height <= 0
        || desc.width > UINT16_MAX || desc.height > UINT16_MAX
        || !std::isfinite(desc.dpiScale) || desc.dpiScale <= 0.0f) {
        return {-1};
    }
    const detail::RenderTargetKey key{
        static_cast<uint16_t>(desc.width), static_cast<uint16_t>(desc.height),
        bgfx::TextureFormat::RGBA8, true, 1, true};
    const detail::PooledRenderTargetHandle pooled = _targetPool->acquire(
        key, false, detail::RenderTargetPoolCategory::RetainedUi);
    if (!pooled.isValid()) {
        ++_frame->layerStats.allocationFailures;
        return {-1};
    }

    const RenderTargetHandle handle{_frame->nextTargetId++};
    RenderTargetRef ref;
    ref.desc = desc;
    ref.pooled = pooled;
    ref.textureToken = reinterpret_cast<void*>(_frame->nextHandle++);
    _frame->targetTextures.emplace(ref.textureToken, handle.id);
    _frame->renderTargets.emplace(handle.id, ref);
    return handle;
}

bool UIRenderBackend::ensureRenderTarget(int targetId)
{
    if (!supportsRenderTargets() || _frame == nullptr) return false;
    auto it = _frame->renderTargets.find(targetId);
    if (it == _frame->renderTargets.end()) return false;
    RenderTargetRef& ref = it->second;
    const detail::RenderTargetKey key{
        static_cast<uint16_t>(ref.desc.width), static_cast<uint16_t>(ref.desc.height),
        bgfx::TextureFormat::RGBA8, true, 1, true};
    if (_targetPool->matches(ref.pooled, key)) {
        const bgfx::TextureHandle texture = _targetPool->texture(ref.pooled);
        if (detail::BGFXAdapter::isValid(texture)) return true;
        // A framebuffer attachment can become stale after a backend/device
        // event even when its numeric framebuffer handle still looks valid.
        // Quarantine that lease and allocate fresh storage before painting.
        _targetPool->release(ref.pooled);
        ref.pooled = {};
    }
    ref.pooled = _targetPool->acquire(
        key, false, detail::RenderTargetPoolCategory::RetainedUi);
    if (ref.pooled.isValid()) return true;

    // Long-lived retained layers are the only leases the UI backend can
    // safely sacrifice under pressure. Revoke the least recently composed
    // backing (the public LayerHandle stays stable), then let quarantine and
    // the pool's idle LRU reclaim it on subsequent frames. This frame falls
    // back to immediate rendering if the strict allocation still cannot fit.
    if (releaseLruLayerBacking(targetId)) {
        ref.pooled = _targetPool->acquire(
            key, false, detail::RenderTargetPoolCategory::RetainedUi);
        if (ref.pooled.isValid()) return true;
    }
    ++_frame->layerStats.allocationFailures;
    return false;
}

bool UIRenderBackend::releaseLruLayerBacking(int excludeTargetId)
{
    if (_frame == nullptr || _targetPool == nullptr) return false;
    LayerRef* victim = nullptr;
    RenderTargetRef* victimTarget = nullptr;
    for (auto& [layerId, layer] : _frame->layers) {
        AYUNREFERENCED_PARAM(layerId);
        if (layer.painting || layer.target.id == excludeTargetId) continue;
        auto target = _frame->renderTargets.find(layer.target.id);
        if (target == _frame->renderTargets.end()
            || !_targetPool->isValid(target->second.pooled)) {
            continue;
        }
        if (victim == nullptr
            || layer.lastCompositeFrame < victim->lastCompositeFrame) {
            victim = &layer;
            victimTarget = &target->second;
        }
    }
    if (victim == nullptr || victimTarget == nullptr) return false;
    _targetPool->release(victimTarget->pooled);
    victimTarget->pooled = {};
    victim->dirty = true;
    victim->damage = {};
    ++_frame->layerStats.degradedLayers;
    return true;
}

bool UIRenderBackend::resizeRenderTarget(RenderTargetHandle target,
                                         const RenderTargetDesc& desc)
{
    if (_frame == nullptr || desc.width <= 0 || desc.height <= 0
        || desc.width > UINT16_MAX || desc.height > UINT16_MAX
        || !std::isfinite(desc.dpiScale) || desc.dpiScale <= 0.0f) {
        return false;
    }
    auto it = _frame->renderTargets.find(target.id);
    if (it == _frame->renderTargets.end()) return false;
    RenderTargetRef& ref = it->second;
    const bool shapeChanged = ref.desc.width != desc.width
        || ref.desc.height != desc.height || ref.desc.hasAlpha != desc.hasAlpha;
    if (!shapeChanged) {
        ref.desc = desc;
        return ensureRenderTarget(target.id);
    }
    if (desc.preserveContents) return false;

    const detail::RenderTargetKey key{
        static_cast<uint16_t>(desc.width), static_cast<uint16_t>(desc.height),
        bgfx::TextureFormat::RGBA8, true, 1, true};
    const detail::PooledRenderTargetHandle replacement = _targetPool->acquire(
        key, false, detail::RenderTargetPoolCategory::RetainedUi);
    if (!replacement.isValid()) return false;
    _targetPool->release(ref.pooled);
    ref.pooled = replacement;
    ref.desc = desc;
    return true;
}

void UIRenderBackend::releaseRenderTarget(RenderTargetHandle target)
{
    if (_frame == nullptr) return;
    auto it = _frame->renderTargets.find(target.id);
    if (it == _frame->renderTargets.end()) return;
    if (_targetPool != nullptr) _targetPool->release(it->second.pooled);
    _frame->targetTextures.erase(it->second.textureToken);
    _frame->renderTargets.erase(it);
    if (_frame->boundTarget.id == target.id) bindRenderTarget({-1});
}

Renderer* UIRenderBackend::previewRenderer() noexcept {
    return _initialized && _frame ? _frame->renderer : nullptr;
}

bool UIRenderBackend::renderScenePreview(RenderTargetHandle target,
    const RenderScene& scene, const PreviewSceneCamera& camera, uint32_t clearRgba,
    const std::function<void(uint16_t)>& beforeDraw)
{
    if (!_initialized || !_frame || !_frame->renderer || !_targetPool
        || _frame->activeLayer.isValid() || _frame->boundTarget.isValid()
        || _frame->nextOffscreenViewId + 3u > kLastLayerViewId
        || !ensureRenderTarget(target.id)) return false;
    const auto found = _frame->renderTargets.find(target.id);
    if (found == _frame->renderTargets.end()) return false;
    const auto fbo = _targetPool->framebuffer(found->second.pooled);
    if (!detail::BGFXAdapter::isValid(fbo)) return false;
    flushColoredRects();
    const auto first = static_cast<uint8_t>(_frame->nextOffscreenViewId);
    _frame->nextOffscreenViewId += 4;
    if (beforeDraw) beforeDraw(first);
    _frame->renderer->renderPreviewScene(scene, camera, fbo.idx,
        static_cast<uint16_t>(found->second.desc.width),
        static_cast<uint16_t>(found->second.desc.height), first, clearRgba);
    return true;
}

void UIRenderBackend::bindRenderTarget(RenderTargetHandle target)
{
    if (_frame == nullptr || _adapter == nullptr || _frame->activeLayer.isValid()) return;
    FrameState& frame = *_frame;
    if (!target.isValid()) {
        if (!frame.boundTarget.isValid()) return;
        flushColoredRects();
        frame.boundTarget = {-1};
        frame.activeViewId = kViewId;
        frame.activeWidth = _width;
        frame.activeHeight = _height;
        frame.originX = frame.originY = 0.0f;
        frame.uiScale = frame.savedUiScale;
        frame.clipStack = std::move(frame.savedClipStack);
        frame.opacityStack = std::move(frame.savedOpacityStack);
        frame.pathClipDepth = frame.savedPathClipDepth;
        frame.currentBlend = frame.savedBlend;
        return;
    }
    if (frame.boundTarget.isValid() || frame.nextOffscreenViewId > kLastLayerViewId
        || !ensureRenderTarget(target.id)) return;
    auto it = frame.renderTargets.find(target.id);
    if (it == frame.renderTargets.end()) return;
    flushColoredRects();
    frame.savedClipStack = frame.clipStack;
    frame.savedOpacityStack = frame.opacityStack;
    frame.savedPathClipDepth = frame.pathClipDepth;
    frame.savedBlend = frame.currentBlend;
    frame.savedUiScale = frame.uiScale;
    frame.clipStack.clear();
    frame.opacityStack.assign(1, 1.0f);
    frame.pathClipDepth = 0;
    frame.currentBlend = ayt::ui::BlendMode::Normal;
    frame.boundTarget = target;
    frame.activeViewId = static_cast<uint8_t>(frame.nextOffscreenViewId++);
    frame.activeWidth = static_cast<uint16_t>(it->second.desc.width);
    frame.activeHeight = static_cast<uint16_t>(it->second.desc.height);
    frame.uiScale = it->second.desc.dpiScale;
    frame.originX = frame.originY = 0.0f;
    _adapter->setViewFrameBuffer(frame.activeViewId,
        _targetPool->framebuffer(it->second.pooled));
    _adapter->setViewRect(frame.activeViewId, 0, 0, frame.activeWidth, frame.activeHeight);
    _adapter->setViewMode(frame.activeViewId, bgfx::ViewMode::Sequential);
    _adapter->setViewClearRaw(frame.activeViewId, BGFX_CLEAR_STENCIL, 0, 1.0f, 0);
    _adapter->touch(frame.activeViewId);
}

void* UIRenderBackend::getRenderTargetTexture(RenderTargetHandle target)
{
    if (_frame == nullptr) return nullptr;
    auto it = _frame->renderTargets.find(target.id);
    if (it == _frame->renderTargets.end() || !ensureRenderTarget(target.id)) return nullptr;
    return it->second.textureToken;
}

void UIRenderBackend::blitRenderTarget(RenderTargetHandle source,
                                       const ayt::math::FRectangle& destBounds)
{
    if (_frame == nullptr || !ensureRenderTarget(source.id)) return;
    const auto it = _frame->renderTargets.find(source.id);
    if (it == _frame->renderTargets.end()) return;
    const bgfx::TextureHandle texture = _targetPool->texture(it->second.pooled);
    if (!detail::BGFXAdapter::isValid(texture)) return;
    const bool flipV = _adapter != nullptr && _adapter->capsOriginBottomLeft();
    emitClippedTexturedQuad(destBounds, texture.idx,
        ayt::math::FRectangle(0, flipV ? 1.0f : 0.0f,
                             1, flipV ? 0.0f : 1.0f),
        ayt::math::FVector4(1, 1, 1, 1));
}

UIRenderBackend::LayerHandle UIRenderBackend::createLayer(const LayerDesc& desc)
{
    const float logicalWidth = desc.logicalBounds.maxX - desc.logicalBounds.minX;
    const float logicalHeight = desc.logicalBounds.maxY - desc.logicalBounds.minY;
    if (!supportsRenderTargets() || logicalWidth <= 0.0f || logicalHeight <= 0.0f
        || !std::isfinite(desc.dpiScale) || desc.dpiScale <= 0.0f) {
        return {-1};
    }
    const ayt::math::FRectangle backingBounds =
        pixelAlignedLayerBounds(desc.logicalBounds, desc.dpiScale);
    RenderTargetDesc targetDesc;
    targetDesc.width = std::max(1, static_cast<int>(std::lround(
        (backingBounds.maxX - backingBounds.minX) * desc.dpiScale)));
    targetDesc.height = std::max(1, static_cast<int>(std::lround(
        (backingBounds.maxY - backingBounds.minY) * desc.dpiScale)));
    targetDesc.dpiScale = desc.dpiScale;
    targetDesc.hasAlpha = desc.hasAlpha;
    targetDesc.preserveContents = false;
    // Layer handles are logical and survive budget pressure. Allocate the
    // backing lazily in beginLayerPaint so failure can use the immediate
    // fallback without destroying/recreating public state every frame.
    const RenderTargetHandle target{_frame->nextTargetId++};
    RenderTargetRef targetRef;
    targetRef.desc = targetDesc;
    targetRef.textureToken = reinterpret_cast<void*>(_frame->nextHandle++);
    _frame->targetTextures.emplace(targetRef.textureToken, target.id);
    _frame->renderTargets.emplace(target.id, targetRef);

    const LayerHandle layer{_frame->nextLayerId++};
    LayerRef ref;
    ref.desc = desc;
    ref.backingBounds = backingBounds;
    ref.target = target;
    _frame->layers.emplace(layer.id, ref);
    ++_frame->layerStats.layerCreates;
    return layer;
}

void UIRenderBackend::releaseLayer(LayerHandle layer)
{
    if (_frame == nullptr) return;
    auto it = _frame->layers.find(layer.id);
    if (it == _frame->layers.end()) return;
    if (_frame->activeLayer.id == layer.id) endLayerPaint(layer);
    const RenderTargetHandle target = it->second.target;
    _frame->layers.erase(it);
    releaseRenderTarget(target);
    ++_frame->layerStats.layerReleases;
}

bool UIRenderBackend::updateLayer(LayerHandle layer, const LayerDesc& desc)
{
    if (_frame == nullptr) return false;
    auto it = _frame->layers.find(layer.id);
    if (it == _frame->layers.end() || it->second.painting) return false;
    const float logicalWidth = desc.logicalBounds.maxX - desc.logicalBounds.minX;
    const float logicalHeight = desc.logicalBounds.maxY - desc.logicalBounds.minY;
    if (logicalWidth <= 0.0f || logicalHeight <= 0.0f
        || !std::isfinite(desc.dpiScale) || desc.dpiScale <= 0.0f) {
        return false;
    }
    const ayt::math::FRectangle backingBounds =
        pixelAlignedLayerBounds(desc.logicalBounds, desc.dpiScale);
    RenderTargetDesc targetDesc;
    targetDesc.width = std::max(1, static_cast<int>(std::lround(
        (backingBounds.maxX - backingBounds.minX) * desc.dpiScale)));
    targetDesc.height = std::max(1, static_cast<int>(std::lround(
        (backingBounds.maxY - backingBounds.minY) * desc.dpiScale)));
    targetDesc.dpiScale = desc.dpiScale;
    targetDesc.hasAlpha = desc.hasAlpha;
    // Layer resize always schedules a full repaint; retaining old pixels is
    // unnecessary and would make resize support backend-dependent.
    targetDesc.preserveContents = false;
    auto target = _frame->renderTargets.find(it->second.target.id);
    if (target == _frame->renderTargets.end()) return false;
    const bool shapeChanged = target->second.desc.width != targetDesc.width
        || target->second.desc.height != targetDesc.height
        || target->second.desc.hasAlpha != targetDesc.hasAlpha;
    if (shapeChanged && _targetPool != nullptr
        && target->second.pooled.isValid()) {
        _targetPool->release(target->second.pooled);
        target->second.pooled = {};
    }
    target->second.desc = targetDesc;
    it->second.desc = desc;
    it->second.backingBounds = backingBounds;
    it->second.dirty = true;
    it->second.damage = {};
    return true;
}

bool UIRenderBackend::beginLayerPaint(LayerHandle layer, const LayerPaint& paint)
{
    if (_frame == nullptr || _adapter == nullptr || _targetPool == nullptr) return false;
    FrameState& frame = *_frame;
    auto it = frame.layers.find(layer.id);
    if (it == frame.layers.end() || frame.activeLayer.isValid()
        || frame.boundTarget.isValid() || frame.nextOffscreenViewId > kLastLayerViewId) {
        return false;
    }
    LayerRef& ref = it->second;
    if (!paint.fullRedraw && !rectNonEmpty(paint.damage)) return false;
    if (!ensureRenderTarget(ref.target.id)) return false;
    auto targetIt = frame.renderTargets.find(ref.target.id);
    if (targetIt == frame.renderTargets.end()) return false;

    flushColoredRects();
    frame.savedClipStack = frame.clipStack;
    frame.savedOpacityStack = frame.opacityStack;
    frame.savedPathClipDepth = frame.pathClipDepth;
    frame.savedBlend = frame.currentBlend;
    frame.savedUiScale = frame.uiScale;
    frame.clipStack.clear();
    frame.opacityStack.assign(1, 1.0f);
    frame.pathClipDepth = 0;
    frame.currentBlend = ayt::ui::BlendMode::Normal;
    frame.uiScale = ref.desc.dpiScale;
    frame.originX = ref.backingBounds.minX;
    frame.originY = ref.backingBounds.minY;
    frame.activeWidth = static_cast<uint16_t>(targetIt->second.desc.width);
    frame.activeHeight = static_cast<uint16_t>(targetIt->second.desc.height);
    frame.activeViewId = static_cast<uint8_t>(frame.nextOffscreenViewId++);
    frame.activeLayer = layer;
    ref.painting = true;
    ref.lastPaintFrame = frame.frameIndex;
    if (paint.fullRedraw) ++frame.layerStats.fullPaints;
    else ++frame.layerStats.partialPaints;
    const ayt::math::FRectangle paintBounds = paint.fullRedraw
        ? ref.desc.logicalBounds : intersectRect(paint.damage, ref.desc.logicalBounds);
    const double physicalArea = std::max(0.0, static_cast<double>(paintBounds.maxX - paintBounds.minX))
        * std::max(0.0, static_cast<double>(paintBounds.maxY - paintBounds.minY))
        * static_cast<double>(ref.desc.dpiScale)
        * static_cast<double>(ref.desc.dpiScale);
    frame.layerStats.repaintPixelArea += static_cast<uint64_t>(std::ceil(physicalArea));

    const bgfx::FrameBufferHandle framebuffer =
        _targetPool->framebuffer(targetIt->second.pooled);
    _adapter->setViewFrameBuffer(frame.activeViewId, framebuffer);
    _adapter->setViewRect(frame.activeViewId, 0, 0, frame.activeWidth, frame.activeHeight);
    _adapter->setViewMode(frame.activeViewId, bgfx::ViewMode::Sequential);
    uint16_t clearFlags = BGFX_CLEAR_STENCIL;
    uint32_t clearColor = 0;
    if (paint.fullRedraw && ref.desc.clearMode != LayerClearMode::Preserve) {
        clearFlags |= BGFX_CLEAR_COLOR;
        if (ref.desc.clearMode == LayerClearMode::Color) {
            clearColor = toRgbaClear(premultipliedLayerColor(ref.desc.clearColor));
        }
    }
    _adapter->setViewClearRaw(frame.activeViewId, clearFlags, clearColor, 1.0f, 0);
    _adapter->touch(frame.activeViewId);

    // Partial Transparent/Color repaint preserves pixels outside damage but
    // must erase stale pixels inside it before replaying the clipped subtree.
    // A no-blend white-texture quad gives us a scissored color clear without
    // changing the view rect/projection. Preserve mode intentionally skips it.
    if (!paint.fullRedraw && ref.desc.clearMode != LayerClearMode::Preserve) {
        const ayt::math::FRectangle damage = intersectRect(
            paint.damage, ref.desc.logicalBounds);
        if (!rectNonEmpty(damage)) {
            endLayerPaint(layer);
            ref.dirty = true;
            return false;
        }
        UiItem clearItem;
        clearItem.textureIdx = _gpu != nullptr ? _gpu->whiteTextureIdx()
                                                : detail::UiGpuContext::kInvalidIdx;
        clearItem.state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
        clearItem.minX = damage.minX;
        clearItem.minY = damage.minY;
        clearItem.maxX = damage.maxX;
        clearItem.maxY = damage.maxY;
        const ayt::math::FVector4 color = ref.desc.clearMode == LayerClearMode::Color
            ? premultipliedLayerColor(ref.desc.clearColor)
            : ayt::math::FVector4(0, 0, 0, 0);
        const uint32_t abgr = toAbgr(color);
        clearItem.abgr[0] = clearItem.abgr[1] = abgr;
        clearItem.abgr[2] = clearItem.abgr[3] = abgr;
        frame.items.push_back(clearItem);
    }
    return true;
}

void UIRenderBackend::endLayerPaint(LayerHandle layer)
{
    if (_frame == nullptr || _frame->activeLayer.id != layer.id) return;
    FrameState& frame = *_frame;
    auto it = frame.layers.find(layer.id);
    if (it == frame.layers.end()) return;
    flushColoredRects();
    it->second.painting = false;
    it->second.dirty = false;
    it->second.damage = {};
    frame.activeLayer = {-1};
    frame.activeViewId = kViewId;
    frame.activeWidth = _width;
    frame.activeHeight = _height;
    frame.originX = frame.originY = 0.0f;
    frame.uiScale = frame.savedUiScale;
    frame.clipStack = std::move(frame.savedClipStack);
    frame.opacityStack = std::move(frame.savedOpacityStack);
    frame.pathClipDepth = frame.savedPathClipDepth;
    frame.currentBlend = frame.savedBlend;
}

void UIRenderBackend::compositeLayer(LayerHandle layer,
                                     const ayt::math::FRectangle& destBounds,
                                     float opacity)
{
    if (_frame == nullptr || _targetPool == nullptr) return;
    auto it = _frame->layers.find(layer.id);
    if (it == _frame->layers.end() || it->second.dirty || it->second.painting) return;
    auto targetIt = _frame->renderTargets.find(it->second.target.id);
    if (targetIt == _frame->renderTargets.end()
        || !_targetPool->isValid(targetIt->second.pooled)) {
        it->second.dirty = true;
        return;
    }
    const bgfx::TextureHandle texture = _targetPool->texture(targetIt->second.pooled);
    if (!detail::BGFXAdapter::isValid(texture)) {
        it->second.dirty = true;
        return;
    }
    const float alpha = std::clamp(opacity, 0.0f, 1.0f);
    const float logicalWidth = it->second.desc.logicalBounds.maxX
        - it->second.desc.logicalBounds.minX;
    const float logicalHeight = it->second.desc.logicalBounds.maxY
        - it->second.desc.logicalBounds.minY;
    const float destWidth = destBounds.maxX - destBounds.minX;
    const float destHeight = destBounds.maxY - destBounds.minY;
    if (logicalWidth <= 0.0f || logicalHeight <= 0.0f
        || destWidth <= 0.0f || destHeight <= 0.0f) {
        return;
    }
    const float scaleX = destWidth / logicalWidth;
    const float scaleY = destHeight / logicalHeight;
    const ayt::math::FRectangle& sourceBounds = it->second.desc.logicalBounds;
    const ayt::math::FRectangle& backingBounds = it->second.backingBounds;
    const ayt::math::FRectangle backingDest(
        destBounds.minX + (backingBounds.minX - sourceBounds.minX) * scaleX,
        destBounds.minY + (backingBounds.minY - sourceBounds.minY) * scaleY,
        destBounds.maxX + (backingBounds.maxX - sourceBounds.maxX) * scaleX,
        destBounds.maxY + (backingBounds.maxY - sourceBounds.maxY) * scaleY);
    // Normal Layer paint stores premultiplied RGB plus source-over coverage
    // alpha. Opacity therefore scales both sampled RGB and alpha, and the
    // composite uses ONE/INV_SRC_ALPHA instead of applying alpha twice.
    // The outward backing can contain transparent padding (or Color-clear
    // pixels). Clip it to the public logical destination while preserving
    // UVs for the full snapped texture; this keeps pixel centres aligned and
    // prevents Color clear from painting outside LayerDesc::logicalBounds.
    pushClip(destBounds);
    const bool flipV = _adapter != nullptr && _adapter->capsOriginBottomLeft();
    emitClippedTexturedQuad(backingDest, texture.idx,
        ayt::math::FRectangle(0, flipV ? 1.0f : 0.0f,
                             1, flipV ? 0.0f : 1.0f),
        ayt::math::FVector4(alpha, alpha, alpha, alpha),
        premultipliedOverStateBits());
    popClip();
    ++_frame->layerStats.composites;
    if (it->second.lastPaintFrame != _frame->frameIndex) {
        ++_frame->layerStats.cacheHits;
    }
    it->second.lastCompositeFrame = _frame->frameIndex;
}

void UIRenderBackend::invalidateLayer(LayerHandle layer,
                                      const ayt::math::FRectangle& damage)
{
    if (_frame == nullptr) return;
    auto it = _frame->layers.find(layer.id);
    if (it == _frame->layers.end()) return;
    LayerRef& ref = it->second;
    ref.dirty = true;
    if (!rectNonEmpty(damage)) {
        ref.damage = {};
    } else if (!rectNonEmpty(ref.damage)) {
        ref.damage = damage;
    } else {
        ref.damage = ayt::math::FRectangle(
            std::min(ref.damage.minX, damage.minX),
            std::min(ref.damage.minY, damage.minY),
            std::max(ref.damage.maxX, damage.maxX),
            std::max(ref.damage.maxY, damage.maxY));
    }
}

bool UIRenderBackend::isLayerDirty(LayerHandle layer) const
{
    if (_frame == nullptr || _targetPool == nullptr) return true;
    const auto it = _frame->layers.find(layer.id);
    if (it == _frame->layers.end() || it->second.dirty) return true;
    const auto targetIt = _frame->renderTargets.find(it->second.target.id);
    if (targetIt == _frame->renderTargets.end()
        || !_targetPool->isValid(targetIt->second.pooled)) {
        return true;
    }
    return !detail::BGFXAdapter::isValid(
        _targetPool->texture(targetIt->second.pooled));
}

UIRenderBackend::LayerCacheStats UIRenderBackend::getLayerCacheStats() const
{
    LayerCacheStats out = _frame != nullptr ? _frame->layerStats : LayerCacheStats{};
    if (_frame != nullptr) {
        out.liveLayers = static_cast<uint32_t>(_frame->layers.size());
    }
    if (_targetPool != nullptr) {
        const detail::RenderTargetPoolStats pool = _targetPool->stats();
        const auto& uiPool = pool.categories[static_cast<size_t>(
            detail::RenderTargetPoolCategory::RetainedUi)];
        out.targetAllocations = uiPool.allocations;
        out.targetReuses = uiPool.reuses;
        out.targetEvictions = pool.evictions;
        out.liveTargetLeases = uiPool.liveLeases;
        out.idleTargets = uiPool.idleTargets;
        out.allocatedTargetBytes = uiPool.allocatedBytes;
        out.targetBudgetBytes = uiPool.budgetBytes;
    }
    return out;
}

void UIRenderBackend::setLayerCacheBudgetBytes(size_t bytes)
{
    if (_targetPool != nullptr) {
        _targetPool->setCategoryBudgetBytes(
            detail::RenderTargetPoolCategory::RetainedUi, bytes);
    }
}

void UIRenderBackend::resetLayerCacheStats()
{
    if (_frame != nullptr) _frame->layerStats = {};
    if (_targetPool != nullptr) _targetPool->resetStats();
}

void UIRenderBackend::setBatchMode(BatchMode mode)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    _frame->batchMode = mode;
}

UIRenderBackend::BatchMode UIRenderBackend::getBatchMode() const
{
    return _frame != nullptr ? _frame->batchMode : BatchMode::OverlapAware;
}

void UIRenderBackend::beginFrame()
{
    _drawCalls = 0;

    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    FrameState& frame = *_frame;
    ++frame.frameIndex;
    if (frame.activeLayer.isValid()) {
        auto active = frame.layers.find(frame.activeLayer.id);
        if (active != frame.layers.end()) {
            active->second.painting = false;
            active->second.dirty = true;
        }
    }
    // Recover from an unbalanced caller before resetting per-frame state.
    // Otherwise the previous transform's derived uiScale would leak into
    // the next frame even though its stack and clips were discarded.
    while (!frame.transformStack.empty()) {
        FrameState::TransformState state =
            std::move(frame.transformStack.back());
        frame.transformStack.pop_back();
        if (state.applied) {
            frame.uiScale = state.uiScale;
            frame.originX = state.originX;
            frame.originY = state.originY;
            frame.clipStack = std::move(state.clipStack);
        }
    }
    frame.items.clear();
    frame.clipStack.clear();
    frame.pathClipDepth = 0;
    frame.hasOrderingBarriers = false;
    frame.currentBlend = ayt::ui::BlendMode::Normal;  // P1: per-frame reset
    // PR-anim: opacity is a per-frame render-state like BlendMode —
    // Widget::render balances its push/pop every frame, but a buggy
    // tree must not leak a stale frame into the next frame.
    frame.opacityStack.assign(1, 1.0f);
    frame.boundTarget = {-1};
    frame.activeLayer = {-1};
    frame.activeViewId = kViewId;
    frame.activeWidth = _width;
    frame.activeHeight = _height;
    frame.nextOffscreenViewId = kFirstLayerViewId;
    frame.originX = frame.originY = 0.0f;
    for (auto& entry : frame.layers) {
        const auto target = frame.renderTargets.find(entry.second.target.id);
        if (target == frame.renderTargets.end() || _targetPool == nullptr
            || !_targetPool->isValid(target->second.pooled)) {
            entry.second.dirty = true;
        }
        entry.second.painting = false;
    }

    if (frame.scratchVertices.capacity() < 1024u) {
        frame.scratchVertices.reserve(1024u);
        frame.scratchIndices.reserve(1536u);
    }

    if (_gpu == nullptr || _width < 1 || _height < 1) {
        return;
    }

    // UI chrome view — fixed high slot (255) so Final PP / bloom can
    // grow without reshuffling menus; must stay > PostProcess (13).
    _gpu->beginView(kViewId, _width, _height);
}

void UIRenderBackend::setUiScale(float scale)
{
    if (_frame == nullptr) _frame = std::make_unique<FrameState>();
    _frame->uiScale = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

float UIRenderBackend::getUiScale() const
{
    return _frame != nullptr ? _frame->uiScale : 1.0f;
}

void UIRenderBackend::beginCanvas(const ayt::math::FRectangle& viewport)
{
    AYUNREFERENCED_PARAM(viewport);
}

void UIRenderBackend::endCanvas() {}

ayt::math::FRectangle UIRenderBackend::activeClipBounds() const
{
    if (_frame == nullptr || _frame->clipStack.empty()) {
        const float scale = getUiScale();
        return ayt::math::FRectangle(
            _frame != nullptr ? _frame->originX : 0.0f,
            _frame != nullptr ? _frame->originY : 0.0f,
            (_frame != nullptr ? _frame->originX : 0.0f)
                + static_cast<float>(_frame != nullptr ? _frame->activeWidth : _width) / scale,
            (_frame != nullptr ? _frame->originY : 0.0f)
                + static_cast<float>(_frame != nullptr ? _frame->activeHeight : _height) / scale);
    }
    return _frame->clipStack.back().bounds;
}

bool UIRenderBackend::clipRect(ayt::math::FRectangle& inout) const
{
    if (_frame == nullptr || _frame->clipStack.empty()) {
        return rectNonEmpty(inout);
    }
    inout = intersectRect(inout, _frame->clipStack.back().bounds);
    return rectNonEmpty(inout);
}

void UIRenderBackend::pushClip(const ayt::math::FRectangle& bounds)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    // P0: no forced flush — items keep array order (z-order), clip is
    // applied at item-record time via clipRect() CPU intersection.

    ayt::math::FRectangle clipped = bounds;
    if (!_frame->clipStack.empty()) {
        clipped = intersectRect(bounds, _frame->clipStack.back().bounds);
    }
    _frame->clipStack.push_back({clipped, false, nullptr, _frame->pathClipDepth});
}

void UIRenderBackend::popClip()
{
    if (_frame == nullptr || _frame->clipStack.empty()) {
        return;
    }
    UiClipEntry entry = std::move(_frame->clipStack.back());
    _frame->clipStack.pop_back();
    if (entry.pathClip) {
        UiItem item;
        item.kind = UiItemKind::PathClipPop;
        item.path = std::move(entry.path);
        item.stencilDepth = entry.depth;
        if (item.path != nullptr && item.path->hasBounds) {
            item.minX = item.path->bounds.minX;
            item.minY = item.path->bounds.minY;
            item.maxX = item.path->bounds.maxX;
            item.maxY = item.path->bounds.maxY;
        }
        _frame->items.push_back(std::move(item));
        if (_frame->pathClipDepth > 0) --_frame->pathClipDepth;
        _frame->hasOrderingBarriers = true;
    }
}

UIRenderBackend::PathHandle UIRenderBackend::createPath()
{
    if (_frame == nullptr) _frame = std::make_unique<FrameState>();
    int id = _frame->nextPathId++;
    if (id <= 0) {
        _frame->nextPathId = 2;
        id = 1;
    }
    _frame->paths.emplace(id, UiPathData{});
    return PathHandle{id};
}

void UIRenderBackend::releasePath(PathHandle path)
{
    if (_frame != nullptr) _frame->paths.erase(path.id);
}

void UIRenderBackend::addPathRect(PathHandle path,
                                  const ayt::math::FRectangle& bounds,
                                  PathWinding winding)
{
    if (_frame == nullptr || !rectNonEmpty(bounds)) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    it->second.contours.push_back({{
        {bounds.minX, bounds.minY}, {bounds.maxX, bounds.minY},
        {bounds.maxX, bounds.maxY}, {bounds.minX, bounds.maxY}
    }, winding, true});
}

void UIRenderBackend::addPathRoundedRect(PathHandle path,
                                         const ayt::math::FRectangle& bounds,
                                         float cornerRadius,
                                         PathWinding winding)
{
    if (_frame == nullptr || !rectNonEmpty(bounds)) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    const float radius = std::clamp(cornerRadius, 0.0f,
        std::min(bounds.maxX - bounds.minX, bounds.maxY - bounds.minY) * 0.5f);
    if (radius <= kPathEpsilon) {
        addPathRect(path, bounds, winding);
        return;
    }

    std::vector<ayt::math::FVector2> points;
    const ayt::math::FVector2 centers[4] = {
        {bounds.maxX - radius, bounds.minY + radius},
        {bounds.maxX - radius, bounds.maxY - radius},
        {bounds.minX + radius, bounds.maxY - radius},
        {bounds.minX + radius, bounds.minY + radius}
    };
    const float starts[4] = {-kPi * 0.5f, 0.0f, kPi * 0.5f, kPi};
    for (int corner = 0; corner < 4; ++corner) {
        auto arc = sampleArc(centers[corner], radius, radius,
                             starts[corner], starts[corner] + kPi * 0.5f,
                             true);
        if (!points.empty() && !arc.empty()) arc.erase(arc.begin());
        points.insert(points.end(), arc.begin(), arc.end());
    }
    it->second.contours.push_back({std::move(points), winding, true});
}

void UIRenderBackend::addPathEllipse(PathHandle path,
                                     const ayt::math::FVector2& center,
                                     float radiusX, float radiusY,
                                     PathWinding winding)
{
    if (_frame == nullptr || radiusX <= 0.0f || radiusY <= 0.0f) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    it->second.contours.push_back({
        sampleArc(center, radiusX, radiusY, 0.0f, kPi * 2.0f, false),
        winding, true});
}

void UIRenderBackend::addPathLine(PathHandle path,
                                  const ayt::math::FVector2& start,
                                  const ayt::math::FVector2& end)
{
    if (_frame == nullptr) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    it->second.contours.push_back({{start, end}, PathWinding::CounterClockwise, false});
}

void UIRenderBackend::addPathBezier(PathHandle path,
                                    const ayt::math::FVector2& start,
                                    const ayt::math::FVector2& control1,
                                    const ayt::math::FVector2& control2,
                                    const ayt::math::FVector2& end)
{
    if (_frame == nullptr) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    const float lengthEstimate =
        pathLength(control1.x - start.x, control1.y - start.y)
      + pathLength(control2.x - control1.x, control2.y - control1.y)
      + pathLength(end.x - control2.x, end.y - control2.y);
    const int segments = std::clamp(
        static_cast<int>(std::ceil(lengthEstimate / 6.0f)), 8, 128);
    std::vector<ayt::math::FVector2> points;
    points.reserve(static_cast<size_t>(segments) + 1u);
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float u = 1.0f - t;
        const float w0 = u * u * u;
        const float w1 = 3.0f * u * u * t;
        const float w2 = 3.0f * u * t * t;
        const float w3 = t * t * t;
        points.emplace_back(start.x * w0 + control1.x * w1 + control2.x * w2 + end.x * w3,
                            start.y * w0 + control1.y * w1 + control2.y * w2 + end.y * w3);
    }
    it->second.contours.push_back({std::move(points), PathWinding::CounterClockwise, false});
}

void UIRenderBackend::addPathArc(PathHandle path,
                                 const ayt::math::FVector2& center,
                                 float radius, float startAngle,
                                 float endAngle, PathWinding winding)
{
    if (_frame == nullptr || radius <= 0.0f || startAngle == endAngle) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    it->second.contours.push_back({
        sampleArc(center, radius, radius, startAngle, endAngle, true),
        winding, false});
}

void UIRenderBackend::addPathPolygon(PathHandle path,
                                     const ayt::math::FVector2* points,
                                     int count, PathWinding winding)
{
    if (_frame == nullptr || points == nullptr || count < 3) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    std::vector<ayt::math::FVector2> copy(points, points + count);
    it->second.contours.push_back({std::move(copy), winding, true});
}

void UIRenderBackend::addPathContour(PathHandle path,
                                     const ayt::math::FVector2* points,
                                     int count, bool closed,
                                     PathWinding winding)
{
    if (_frame == nullptr || points == nullptr || count < 2) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    std::vector<ayt::math::FVector2> copy(points, points + count);
    it->second.contours.push_back({std::move(copy), winding, closed && count >= 3});
}

void UIRenderBackend::setPathFillColor(PathHandle path,
                                       const ayt::math::FVector4& color)
{
    if (_frame == nullptr) return;
    auto it = _frame->paths.find(path.id);
    if (it != _frame->paths.end()) it->second.fillColor = color;
}

void UIRenderBackend::setPathStrokeColor(PathHandle path,
                                         const ayt::math::FVector4& color)
{
    if (_frame == nullptr) return;
    auto it = _frame->paths.find(path.id);
    if (it != _frame->paths.end()) it->second.strokeColor = color;
}

void UIRenderBackend::setPathStrokeWidth(PathHandle path, float width)
{
    if (_frame == nullptr) return;
    auto it = _frame->paths.find(path.id);
    if (it != _frame->paths.end()) it->second.strokeWidth = std::max(0.0f, width);
}

void UIRenderBackend::setPathStrokeStyle(PathHandle path, PathStrokeCap cap,
                                         PathStrokeJoin join, float miterLimit)
{
    if (_frame == nullptr) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    it->second.strokeCap = cap;
    it->second.strokeJoin = join;
    it->second.miterLimit = std::max(1.0f, miterLimit);
}

void UIRenderBackend::drawPath(PathHandle path, PathFillMode mode)
{
    if (_frame == nullptr) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;
    std::shared_ptr<UiPathSnapshot> snapshot = snapshotPath(it->second, activeClipBounds());
    const bool wantsFill = mode == PathFillMode::Fill || mode == PathFillMode::FillAndStroke;
    const bool wantsStroke = mode == PathFillMode::Stroke || mode == PathFillMode::FillAndStroke;

    bool hasOuterFill = false;
    for (const auto& contour : snapshot->fillContours) {
        if (contour.winding == PathWinding::CounterClockwise) {
            hasOuterFill = true;
            break;
        }
    }
    if (wantsFill && hasOuterFill && snapshot->hasBounds) {
        UiItem item;
        item.kind = UiItemKind::PathFill;
        item.path = snapshot;
        item.state = blendStateBits(_frame->currentBlend);
        item.stencilDepth = _frame->pathClipDepth;
        item.minX = snapshot->bounds.minX;
        item.minY = snapshot->bounds.minY;
        item.maxX = snapshot->bounds.maxX;
        item.maxY = snapshot->bounds.maxY;
        ayt::math::FVector4 color = it->second.fillColor;
        color.w *= _frame->opacityStack.back();
        item.abgr[0] = toAbgr(color);
        _frame->items.push_back(std::move(item));
        _frame->hasOrderingBarriers = true;
    }
    if (wantsStroke && !snapshot->strokeMesh.indices.empty()) {
        UiItem item;
        item.kind = UiItemKind::Mesh;
        item.mesh = std::make_shared<UiTriangleMesh>(snapshot->strokeMesh);
        item.state = blendStateBits(_frame->currentBlend);
        item.stencilDepth = _frame->pathClipDepth;
        item.minX = snapshot->bounds.minX;
        item.minY = snapshot->bounds.minY;
        item.maxX = snapshot->bounds.maxX;
        item.maxY = snapshot->bounds.maxY;
        ayt::math::FVector4 color = it->second.strokeColor;
        color.w *= _frame->opacityStack.back();
        item.abgr[0] = toAbgr(color);
        _frame->items.push_back(std::move(item));
    }
}

void UIRenderBackend::pushPathClip(PathHandle path)
{
    if (_frame == nullptr) return;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return;

    // Reserve stencil value 255 for the temporary fill mask used by
    // drawPath().  A saturated clip must still contribute a stack entry so
    // the caller's matching popClip() cannot accidentally remove its parent.
    // Treat the unsupported depth as an empty rectangular clip: descendants
    // are safely rejected on the CPU while the existing stencil depth remains
    // balanced and unchanged.
    if (_frame->pathClipDepth >= UINT8_MAX - 1u) {
        const ayt::math::FRectangle empty(0.0f, 0.0f, 0.0f, 0.0f);
        _frame->clipStack.push_back({empty, false, nullptr, _frame->pathClipDepth});
        return;
    }

    std::shared_ptr<UiPathSnapshot> snapshot = snapshotPath(it->second, activeClipBounds());
    UiItem item;
    item.kind = UiItemKind::PathClipPush;
    item.path = snapshot;
    item.stencilDepth = _frame->pathClipDepth;
    if (snapshot->hasBounds) {
        item.minX = snapshot->bounds.minX;
        item.minY = snapshot->bounds.minY;
        item.maxX = snapshot->bounds.maxX;
        item.maxY = snapshot->bounds.maxY;
    }
    _frame->items.push_back(std::move(item));
    ++_frame->pathClipDepth;

    ayt::math::FRectangle coarse(0.0f, 0.0f, 0.0f, 0.0f);
    bool hasOuter = false;
    for (const auto& contour : snapshot->fillContours) {
        if (contour.winding == PathWinding::CounterClockwise) {
            hasOuter = true;
            break;
        }
    }
    if (hasOuter && snapshot->hasBounds) coarse = snapshot->bounds;
    _frame->clipStack.push_back({coarse, true, snapshot, _frame->pathClipDepth});
    _frame->hasOrderingBarriers = true;
}

bool UIRenderBackend::getPathDebugInfo(PathHandle path, PathDebugInfo& outInfo) const
{
    outInfo = {};
    if (_frame == nullptr) return false;
    auto it = _frame->paths.find(path.id);
    if (it == _frame->paths.end()) return false;
    const ayt::math::FRectangle unlimited(-1.0e8f, -1.0e8f, 1.0e8f, 1.0e8f);
    const auto snapshot = snapshotPath(it->second, unlimited);
    outInfo.contourCount = it->second.contours.size();
    for (const auto& contour : it->second.contours) {
        if (contour.winding == PathWinding::Clockwise) {
            ++outInfo.clockwiseContourCount;
        }
        if (!contour.closed) ++outInfo.openContourCount;
    }
    for (const auto& contour : snapshot->fillContours) {
        outInfo.fillTriangleCount += contour.mesh.indices.size() / 3u;
    }
    outInfo.strokeTriangleCount = snapshot->strokeMesh.indices.size() / 3u;
    if (snapshot->hasBounds) outInfo.bounds = snapshot->bounds;
    return true;
}

uint8_t UIRenderBackend::getActivePathClipDepthForDebug() const
{
    return _frame != nullptr ? _frame->pathClipDepth : 0u;
}

size_t UIRenderBackend::getPendingUiItemCountForDebug() const
{
    return _frame != nullptr ? _frame->items.size() : 0u;
}

bool UIRenderBackend::hasPathOrderingBarrierForDebug() const
{
    return _frame != nullptr && _frame->hasOrderingBarriers;
}

void UIRenderBackend::endFrame()
{
    if (_frame != nullptr && _frame->activeLayer.isValid()) {
        endLayerPaint(_frame->activeLayer);
    }
    if (_frame != nullptr && _frame->boundTarget.isValid()) {
        bindRenderTarget({-1});
    }
    flushColoredRects();
    flushPendingText();
}

void UIRenderBackend::flushBatches()
{
    // P0: unified batch — flush submits everything recorded so far.
    flushColoredRects();
}

void UIRenderBackend::syncTextAtlasIfNeeded(ayt::font::IFont* font)
{
    if (_fontAtlas == nullptr || !_fontAtlas->isAtlasDirty(font) || _frame == nullptr) {
        return;
    }
    if (font == nullptr) {
        return;
    }

    _fontAtlas->syncAtlasToGpu(font);
}

void UIRenderBackend::drawRect(const ayt::math::FRectangle& bounds, const ayt::math::FVector4& color)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // P0 unified batch: append item; z-order is preserved by array order
    // (no mid-frame flushes — the old "cut the text batch" flush here
    // existed only to keep draw order across two separate batches).
    UiItem item;
    item.textureIdx = (_gpu != nullptr) ? _gpu->whiteTextureIdx()
                                        : detail::UiGpuContext::kInvalidIdx;
    item.state = blendStateBits(_frame->currentBlend);  // P1
    item.minX = clipped.minX;
    item.minY = clipped.minY;
    item.maxX = clipped.maxX;
    item.maxY = clipped.maxY;
    // PR-anim: flat fill fades with the tree.
    ayt::math::FVector4 fill = color;
    fill.w *= _frame->opacityStack.back();
    const uint32_t abgr = toAbgr(fill);
    item.abgr[0] = abgr;
    item.abgr[1] = abgr;
    item.abgr[2] = abgr;
    item.abgr[3] = abgr;
    item.stencilDepth = _frame->pathClipDepth;
    _frame->items.push_back(item);
}

void UIRenderBackend::setBlendMode(ayt::ui::BlendMode mode)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    // Recorded into every item's state at draw time; a mode change starts
    // a new flush run. Reset to Normal at beginFrame.
    _frame->currentBlend = mode;
}

void UIRenderBackend::pushOpacity(float alpha)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    // Clamp to [0,1]: 1.0 is a no-op, out-of-range values are caller
    // bugs that would otherwise stack nonsense (alpha > 1 makes draws
    // opaque-er than authored, negative alpha breaks the multiply).
    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    // COMPOUND: the stack stores the CUMULATIVE product, not the raw
    // frame — parent push(0.5) then child push(0.5) yields stack top
    // 0.25 (tree opacity multiplies). pop() restores the parent frame.
    _frame->opacityStack.push_back(_frame->opacityStack.back() * a);
}

void UIRenderBackend::popOpacity()
{
    if (_frame == nullptr) {
        return;
    }
    // The stack base (1.0) is never popped — an unbalanced pop must not
    // empty the stack and corrupt the rest of the frame.
    if (_frame->opacityStack.size() > 1) {
        _frame->opacityStack.pop_back();
    }
}

void UIRenderBackend::pushTransform(const ayt::math::Float4x4& transform)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    FrameState& frame = *_frame;
    FrameState::TransformState saved;

    const float sx = transform(0, 0);
    const float sy = transform(1, 1);
    const float tx = transform(0, 3);
    const float ty = transform(1, 3);
    const auto nearZero = [](float value) { return std::fabs(value) < 0.0001f; };
    bool supported = std::isfinite(sx) && std::isfinite(sy) &&
        std::isfinite(tx) && std::isfinite(ty) && sx > 0.0001f &&
        std::fabs(sx - sy) < 0.0001f && nearZero(transform(0, 1)) &&
        nearZero(transform(1, 0)) && nearZero(transform(0, 2)) &&
        nearZero(transform(1, 2)) && nearZero(transform(2, 0)) &&
        nearZero(transform(2, 1)) && nearZero(transform(3, 0)) &&
        nearZero(transform(3, 1)) && nearZero(transform(3, 2));
    if (supported) {
        for (const UiClipEntry& clip : frame.clipStack) {
            if (clip.pathClip) {
                supported = false;
                break;
            }
        }
    }
    if (!supported) {
        frame.transformStack.push_back(std::move(saved));
        return;
    }

    // Items are converted to physical coordinates only at flush time, so a
    // state change needs a batch boundary. This costs one boundary per
    // transformed subtree, not one per widget or primitive.
    flushColoredRects();
    saved.applied = true;
    saved.uiScale = frame.uiScale;
    saved.originX = frame.originX;
    saved.originY = frame.originY;
    saved.clipStack = frame.clipStack;
    frame.transformStack.push_back(std::move(saved));

    const float inverse = 1.0f / sx;
    for (UiClipEntry& clip : frame.clipStack) {
        clip.bounds = ayt::math::FRectangle(
            (clip.bounds.minX - tx) * inverse,
            (clip.bounds.minY - ty) * inverse,
            (clip.bounds.maxX - tx) * inverse,
            (clip.bounds.maxY - ty) * inverse);
    }
    frame.originX = (frame.originX - tx) * inverse;
    frame.originY = (frame.originY - ty) * inverse;
    frame.uiScale *= sx;
}

void UIRenderBackend::popTransform()
{
    if (_frame == nullptr || _frame->transformStack.empty()) {
        return;
    }
    FrameState& frame = *_frame;
    FrameState::TransformState saved =
        std::move(frame.transformStack.back());
    frame.transformStack.pop_back();
    if (!saved.applied) return;

    flushColoredRects();
    frame.uiScale = saved.uiScale;
    frame.originX = saved.originX;
    frame.originY = saved.originY;
    frame.clipStack = std::move(saved.clipStack);
}

void UIRenderBackend::drawGradientRect(const ayt::math::FRectangle& bounds,
                                       const ayt::math::FVector4& topColor,
                                       const ayt::math::FVector4& bottomColor)
{
    // 2-color vertical gradient — routes to the 4-color path
    // (topColor = both top corners, bottomColor = both bottom
    // corners), matching AYUI MockRenderer semantics.
    drawGradientRect(bounds, topColor, topColor, bottomColor, bottomColor);
}

void UIRenderBackend::drawGradientRect(const ayt::math::FRectangle& bounds,
                                       const ayt::math::FVector4& topLeft,
                                       const ayt::math::FVector4& topRight,
                                       const ayt::math::FVector4& bottomLeft,
                                       const ayt::math::FVector4& bottomRight)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // P1: a Flat item with per-corner colors — the existing shader
    // interpolates v_color0 across the quad, so gradients are free.
    // When clipping crops the quad, remap its corner colors against the
    // original bounds as well. Reusing the authored corner colors on the
    // smaller clipped quad would restart/stretch the gradient inside a
    // damage rect instead of revealing the matching part of the original.
    const float width = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 1.0e-5f || height <= 1.0e-5f) return;
    const float left = std::clamp((clipped.minX - bounds.minX) / width, 0.0f, 1.0f);
    const float right = std::clamp((clipped.maxX - bounds.minX) / width, 0.0f, 1.0f);
    const float top = std::clamp((clipped.minY - bounds.minY) / height, 0.0f, 1.0f);
    const float bottom = std::clamp((clipped.maxY - bounds.minY) / height, 0.0f, 1.0f);
    const auto lerpColor = [](const ayt::math::FVector4& a,
                              const ayt::math::FVector4& b, float t) {
        return ayt::math::FVector4(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t);
    };
    const auto sampleColor = [&](float x, float y) {
        return lerpColor(lerpColor(topLeft, topRight, x),
                         lerpColor(bottomLeft, bottomRight, x), y);
    };

    // Corner order matches flushFlatRun: TL TR BR BL.
    UiItem item;
    item.textureIdx = (_gpu != nullptr) ? _gpu->whiteTextureIdx()
                                        : detail::UiGpuContext::kInvalidIdx;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX = clipped.minX;
    item.minY = clipped.minY;
    item.maxX = clipped.maxX;
    item.maxY = clipped.maxY;
    // PR-anim: gradient corners fade with the tree.
    ayt::math::FVector4 tl = sampleColor(left, top);
    ayt::math::FVector4 tr = sampleColor(right, top);
    ayt::math::FVector4 bl = sampleColor(left, bottom);
    ayt::math::FVector4 br = sampleColor(right, bottom);
    const float op = _frame->opacityStack.back();
    tl.w *= op; tr.w *= op; bl.w *= op; br.w *= op;
    item.abgr[0] = toAbgr(tl);
    item.abgr[1] = toAbgr(tr);
    item.abgr[2] = toAbgr(br);
    item.abgr[3] = toAbgr(bl);
    item.stencilDepth = _frame->pathClipDepth;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawBorderRect(const ayt::math::FRectangle& bounds,
                                     const ayt::math::FVector4& color,
                                     float borderWidth, float cornerRadius)
{
    drawBorderRect(bounds, color, borderWidth,
                   ayt::ui::IRenderBackend::CornerRadii(cornerRadius));
}

void UIRenderBackend::drawBorderRect(const ayt::math::FRectangle& bounds,
                                     const ayt::math::FVector4& color,
                                     float borderWidth,
                                     const ayt::ui::IRenderBackend::CornerRadii& radii)
{
    // P2: single SDF item — the shader renders the ring natively with
    // per-pixel AA (the P1 body's 8-rect decomposition made square corners).
    if (borderWidth <= 0.0f) {
        return;
    }

    const float width  = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    const float w = std::min(borderWidth, std::min(width, height) * 0.5f);
    if (w <= 0.0f) {
        return;
    }

    // Degenerate: border as wide as the rect is a solid fill (same
    // fallback the interface default used).
    if (width <= w * 2.0f || height <= w * 2.0f) {
        drawRect(bounds, color);
        return;
    }

    // Shape stays the caller bounds. Only the *draw quad* is intersected
    // with the clip — reshaping sdf.rect to the clipped remnant invents
    // four new corners that "stick" to the scroll/clip edge.
    ayt::math::FRectangle drawQuad(bounds.minX - w, bounds.minY - w,
                                   bounds.maxX + w, bounds.maxY + w);
    if (!clipRect(drawQuad)) {
        return;
    }

    const float maxRadius = std::min(width, height) * 0.5f;

    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = drawQuad.minX;
    item.minY  = drawQuad.minY;
    item.maxX  = drawQuad.maxX;
    item.maxY  = drawQuad.maxY;
    item.shapeMinX = bounds.minX;
    item.shapeMinY = bounds.minY;
    item.shapeMaxX = bounds.maxX;
    item.shapeMaxY = bounds.maxY;
    item.sdf.radius = radiiVec4(radii, maxRadius);
    // PR-anim: stroke fades with the tree (shader treats a=0 as no stroke).
    ayt::math::FVector4 stroke = color;
    stroke.w *= _frame->opacityStack.back();
    item.sdf.strokeColor = stroke;
    item.sdf.strokeWidth = w;
    // Center ring (inset 0) — the legacy drawBorderRect visual; drawCard
    // routes BorderStyle::Position through strokeInset instead.
    item.sdf.strokeInset = 0.0f;
    // Soft-clip: the clip rect at record time, for seam AA in the shader.
    const ayt::math::FRectangle clip = activeClipBounds();
    item.clipMinX = clip.minX;
    item.clipMinY = clip.minY;
    item.clipMaxX = clip.maxX;
    item.clipMaxY = clip.maxY;
    item.stencilDepth = _frame->pathClipDepth;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawRoundedRect(const ayt::math::FRectangle& bounds,
                                      const ayt::math::FVector4& color,
                                      float cornerRadius)
{
    drawRoundedRect(bounds, color, ayt::ui::IRenderBackend::CornerRadii(cornerRadius));
}

void UIRenderBackend::drawRoundedRect(const ayt::math::FRectangle& bounds,
                                      const ayt::math::FVector4& color,
                                      const ayt::ui::IRenderBackend::CornerRadii& radii)
{
    // SDF filled rounded rect — pairs with drawBorderRect so combo cards
    // (fill + stroke) share the same silhouette. Flat drawRect is axis-
    // aligned and pokes square corners through a rounded stroke.
    if (color.w <= 0.0f) {
        return;
    }

    const float width  = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    const float maxRadius = std::min(width, height) * 0.5f;
    const ayt::math::FVector4 r = radiiVec4(radii, maxRadius);
    if (r.x <= 0.0f && r.y <= 0.0f && r.z <= 0.0f && r.w <= 0.0f) {
        drawRect(bounds, color);
        return;
    }

    constexpr float kAa = 1.0f;
    ayt::math::FRectangle drawQuad(bounds.minX - kAa, bounds.minY - kAa,
                                   bounds.maxX + kAa, bounds.maxY + kAa);
    if (!clipRect(drawQuad)) {
        return;
    }

    // PR-anim: SDF fill fades with the tree (per-vertex color rides alpha).
    ayt::math::FVector4 fillColor = color;
    fillColor.w *= _frame->opacityStack.back();
    const uint32_t fill = toAbgr(fillColor);
    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = drawQuad.minX;
    item.minY  = drawQuad.minY;
    item.maxX  = drawQuad.maxX;
    item.maxY  = drawQuad.maxY;
    item.abgr[0] = fill;
    item.abgr[1] = fill;
    item.abgr[2] = fill;
    item.abgr[3] = fill;
    // Original silhouette — do not reshape to the clipped remnant.
    item.shapeMinX = bounds.minX;
    item.shapeMinY = bounds.minY;
    item.shapeMaxX = bounds.maxX;
    item.shapeMaxY = bounds.maxY;
    item.sdf.radius = r;
    const ayt::math::FRectangle clip = activeClipBounds();
    item.clipMinX = clip.minX;
    item.clipMinY = clip.minY;
    item.clipMaxX = clip.maxX;
    item.clipMaxY = clip.maxY;
    item.stencilDepth = _frame->pathClipDepth;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawRectShadow(const ayt::math::FRectangle& bounds,
                                     const ayt::ui::IRenderBackend::ShadowStyle& shadow)
{
    // P2: single SDF item; blur softens coverage over `blur` pixels in the
    // shader (d/blur), not by expanding the shape rect. The draw quad grows
    // by |offset| + blur so the falloff is not clipped. The shape fields
    // stay the *content* bounds — using the expanded outer made solid
    // black blobs.
    if (shadow.color.w <= 0.0f) {
        return;
    }

    const float width  = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    const float blur = std::max(0.0f, shadow.blurRadius);
    const float extX = std::fabs(shadow.offset.x) + blur;
    const float extY = std::fabs(shadow.offset.y) + blur;

    ayt::math::FRectangle drawQuad(bounds.minX - extX, bounds.minY - extY,
                                   bounds.maxX + extX, bounds.maxY + extY);
    if (!clipRect(drawQuad)) {
        return;
    }

    const float maxRadius = std::min(width, height) * 0.5f;
    const float r         = std::max(0.0f, std::min(shadow.cornerRadius, maxRadius));

    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = drawQuad.minX;
    item.minY  = drawQuad.minY;
    item.maxX  = drawQuad.maxX;
    item.maxY  = drawQuad.maxY;
    item.shapeMinX = bounds.minX;
    item.shapeMinY = bounds.minY;
    item.shapeMaxX = bounds.maxX;
    item.shapeMaxY = bounds.maxY;
    item.sdf.radius       = ayt::math::FVector4(r, r, r, r);
    // PR-anim: shadow fades with the tree (a=0 = no shadow in shader).
    ayt::math::FVector4 shadowColor = shadow.color;
    shadowColor.w *= _frame->opacityStack.back();
    item.sdf.shadowColor  = shadowColor;
    item.sdf.shadowOffset = shadow.offset;
    item.sdf.shadowBlur   = blur;
    const ayt::math::FRectangle clip = activeClipBounds();
    item.clipMinX = clip.minX;
    item.clipMinY = clip.minY;
    item.clipMaxX = clip.maxX;
    item.clipMaxY = clip.maxY;
    item.stencilDepth = _frame->pathClipDepth;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawCard(const ayt::math::FRectangle& bounds,
                               const ayt::ui::IRenderBackend::CardStyle& style)
{
    // Single SDF item: the shader composites shadow → fill → stroke in one
    // pass, so a card that used to cost drawRectShadow + drawRoundedRect +
    // drawBorderRect now submits once (same silhouette for all three layers
    // — no corner AA mismatch between fill and ring).
    const bool hasFill   = style.fillColor.w > 0.0f;
    const bool hasStroke = style.borderWidth > 0.0f && style.borderColor.w > 0.0f;
    const bool hasShadow = style.shadowColor.w > 0.0f;
    if (!hasFill && !hasStroke && !hasShadow) {
        return;
    }

    const float width  = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    const float maxRadius = std::min(width, height) * 0.5f;
    // Stroke width clamp matches drawBorderRect; the ring band for any
    // position (|inset| <= w/2) stays within w px of the shape, so the
    // draw quad grows by w at most.
    const float strokeW = hasStroke ? std::min(style.borderWidth, maxRadius) : 0.0f;
    const float blur    = hasShadow ? std::max(0.0f, style.shadowBlurRadius) : 0.0f;

    const float extX = std::max(hasShadow ? std::fabs(style.shadowOffset.x) + blur : 0.0f,
                                strokeW);
    const float extY = std::max(hasShadow ? std::fabs(style.shadowOffset.y) + blur : 0.0f,
                                strokeW);
    constexpr float kAa = 1.0f;  // SDF AA margin (cover() falloff is 1px)
    ayt::math::FRectangle drawQuad(bounds.minX - extX - kAa, bounds.minY - extY - kAa,
                                   bounds.maxX + extX + kAa, bounds.maxY + extY + kAa);
    if (!clipRect(drawQuad)) {
        return;
    }

    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = drawQuad.minX;
    item.minY  = drawQuad.minY;
    item.maxX  = drawQuad.maxX;
    item.maxY  = drawQuad.maxY;
    item.shapeMinX = bounds.minX;
    item.shapeMinY = bounds.minY;
    item.shapeMaxX = bounds.maxX;
    item.shapeMaxY = bounds.maxY;
    item.sdf.radius = radiiVec4(style.cornerRadius, maxRadius);

    // PR-anim: every layer fades with the tree (a=0 disables the layer).
    if (hasFill) {
        ayt::math::FVector4 fill = style.fillColor;
        fill.w *= _frame->opacityStack.back();
        const uint32_t abgr = toAbgr(fill);
        item.abgr[0] = abgr;
        item.abgr[1] = abgr;
        item.abgr[2] = abgr;
        item.abgr[3] = abgr;
    }
    if (hasStroke) {
        ayt::math::FVector4 stroke = style.borderColor;
        stroke.w *= _frame->opacityStack.back();
        item.sdf.strokeColor = stroke;
        item.sdf.strokeWidth = strokeW;
        item.sdf.strokeInset = borderInsetForPosition(style.borderPosition, strokeW);
    }
    if (hasShadow) {
        ayt::math::FVector4 shadowColor = style.shadowColor;
        shadowColor.w *= _frame->opacityStack.back();
        item.sdf.shadowColor  = shadowColor;
        item.sdf.shadowOffset = style.shadowOffset;
        item.sdf.shadowBlur   = blur;
    }
    const ayt::math::FRectangle clip = activeClipBounds();
    item.clipMinX = clip.minX;
    item.clipMinY = clip.minY;
    item.clipMaxX = clip.maxX;
    item.clipMaxY = clip.maxY;
    item.stencilDepth = _frame->pathClipDepth;
    _frame->items.push_back(item);
}

void* UIRenderBackend::createUiTexture(uint16_t width, uint16_t height, const void* bgraPixels)
{
    if (!_initialized || _gpu == nullptr || _adapter == nullptr || bgraPixels == nullptr
        || width == 0 || height == 0) {
        return nullptr;
    }

    const uint32_t byteSize = static_cast<uint32_t>(static_cast<uint64_t>(width) * height * 4u);
    const uint16_t idx = _gpu->uploadUiTexture(*_adapter, width, height, bgraPixels, byteSize);
    if (idx == detail::UiGpuContext::kInvalidIdx) {
        return nullptr;
    }

    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    void* handle = reinterpret_cast<void*>(_frame->nextHandle++);
    _frame->textures[handle] = TextureRef{idx, 1u, width, height};
    return handle;
}

void UIRenderBackend::releaseUiTexture(void* textureHandle)
{
    if (_frame == nullptr) {
        return;
    }
    auto it = _frame->textures.find(textureHandle);
    if (it == _frame->textures.end()) {
        return;  // unknown / double release — safe no-op
    }
    if (--it->second.refCount == 0) {
        if (_initialized && _gpu != nullptr && _adapter != nullptr) {
            _gpu->releaseTextTexture(*_adapter, it->second.textureIdx);
        }
        _frame->textures.erase(it);
    }
}

bool UIRenderBackend::resolveTextureHandle(void* handle, uint16_t& textureIdx,
                                           uint16_t& width, uint16_t& height) const
{
    textureIdx = detail::UiGpuContext::kInvalidIdx;
    width = height = 0;
    if (_frame == nullptr || handle == nullptr) return false;
    if (const TextureRef* ref = findTextureRef(_frame->textures, handle)) {
        textureIdx = ref->textureIdx;
        width = ref->width;
        height = ref->height;
        return textureIdx != detail::UiGpuContext::kInvalidIdx;
    }
    const auto token = _frame->targetTextures.find(handle);
    if (token == _frame->targetTextures.end() || _targetPool == nullptr) return false;
    const auto target = _frame->renderTargets.find(token->second);
    if (target == _frame->renderTargets.end()
        || !_targetPool->isValid(target->second.pooled)) return false;
    const bgfx::TextureHandle texture = _targetPool->texture(target->second.pooled);
    if (!detail::BGFXAdapter::isValid(texture)) return false;
    textureIdx = texture.idx;
    width = static_cast<uint16_t>(target->second.desc.width);
    height = static_cast<uint16_t>(target->second.desc.height);
    return true;
}

void UIRenderBackend::drawRect(const ayt::math::FRectangle& bounds, void* textureHandle,
                               const ayt::math::FRectangle& uv)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    uint16_t textureIdx = detail::UiGpuContext::kInvalidIdx;
    uint16_t textureWidth = 0;
    uint16_t textureHeight = 0;
    if (!resolveTextureHandle(textureHandle, textureIdx, textureWidth, textureHeight)) {
        // Unknown / released handle — pre-P3 gray-block fallback.
        drawRect(bounds, kUnknownTextureColor);
        return;
    }

    // Flat item on the UI texture with an opaque white tint — the shader
    // multiplies v_color0 by the texture sample, so alpha rides through.
    emitClippedTexturedQuad(bounds, textureIdx, uv,
                            ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
}

bool UIRenderBackend::addTexturedQuad(
    const ayt::math::FRectangle& bounds, void* textureHandle,
    const ayt::math::FRectangle& uv, const ayt::math::FVector4& tint)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    uint16_t textureIdx = detail::UiGpuContext::kInvalidIdx;
    uint16_t textureWidth = 0;
    uint16_t textureHeight = 0;
    if (!resolveTextureHandle(textureHandle, textureIdx,
                              textureWidth, textureHeight)) {
        drawRect(bounds, ayt::math::FVector4(
            kUnknownTextureColor.x * tint.x,
            kUnknownTextureColor.y * tint.y,
            kUnknownTextureColor.z * tint.z,
            kUnknownTextureColor.w * tint.w));
        return false;
    }

    emitClippedTexturedQuad(bounds, textureIdx, uv, tint);
    return true;
}

void UIRenderBackend::drawWithAlpha(const ayt::math::FRectangle& bounds, void* textureHandle,
                                    float alpha)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    uint16_t textureIdx = detail::UiGpuContext::kInvalidIdx;
    uint16_t textureWidth = 0;
    uint16_t textureHeight = 0;
    if (!resolveTextureHandle(textureHandle, textureIdx, textureWidth, textureHeight)) {
        // Unknown handle — gray fallback tinted by the requested alpha.
        drawRect(bounds, ayt::math::FVector4(
                             kUnknownTextureColor.x, kUnknownTextureColor.y,
                             kUnknownTextureColor.z,
                             std::max(0.0f, std::min(1.0f, alpha))));
        return;
    }

    // Interface contract: alpha multiplies the texture's alpha channel —
    // carried by the tint (shader does v_color0 * sample).
    emitClippedTexturedQuad(bounds, textureIdx,
                            ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                            ayt::math::FVector4(
                                1.0f, 1.0f, 1.0f, std::max(0.0f, std::min(1.0f, alpha))));
}

void UIRenderBackend::emitClippedTexturedQuad(const ayt::math::FRectangle& bounds,
                                              uint16_t textureIdx,
                                              const ayt::math::FRectangle& uv,
                                              const ayt::math::FVector4& tint,
                                              uint64_t stateOverride)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // UV remap: the quad is CPU-clipped to the clip rect, so the sampled
    // region must shrink by the same fraction — otherwise the texture
    // smears/stretches across the seam (whole-image UVs on a clipped quad
    // squeeze the edge texels). Correct for LINEAR sampling (UI textures);
    // glyph UVs never route here (they remap in emitGlyph already).
    float u0 = uv.minX, v0 = uv.minY, u1 = uv.maxX, v1 = uv.maxY;
    const float bw = bounds.maxX - bounds.minX;
    const float bh = bounds.maxY - bounds.minY;
    if (bw > 1e-5f && bh > 1e-5f) {
        const float lf = (clipped.minX - bounds.minX) / bw;
        const float rf = (clipped.maxX - bounds.minX) / bw;
        const float tf = (clipped.minY - bounds.minY) / bh;
        const float bf = (clipped.maxY - bounds.minY) / bh;
        u0 = uv.minX + (uv.maxX - uv.minX) * lf;
        u1 = uv.minX + (uv.maxX - uv.minX) * rf;
        v0 = uv.minY + (uv.maxY - uv.minY) * tf;
        v1 = uv.minY + (uv.maxY - uv.minY) * bf;
    }

    // PR-anim: alpha rides the tint (shader does v_color0 * sample);
    // opacity==1 keeps 0xFFFFFFFF.
    const uint32_t abgr = toAbgr(ayt::math::FVector4(
        tint.x, tint.y, tint.z, tint.w * _frame->opacityStack.back()));
    UiItem item;
    item.textureIdx = textureIdx;
    item.state      = stateOverride != 0
        ? stateOverride : blendStateBits(_frame->currentBlend);
    item.minX       = clipped.minX;
    item.minY       = clipped.minY;
    item.maxX       = clipped.maxX;
    item.maxY       = clipped.maxY;
    item.abgr[0] = abgr;
    item.abgr[1] = abgr;
    item.abgr[2] = abgr;
    item.abgr[3] = abgr;
    item.u0 = u0;
    item.v0 = v0;
    item.u1 = u1;
    item.v1 = v1;
    item.stencilDepth = _frame->pathClipDepth;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawNinePatch(const ayt::math::FRectangle& bounds, void* textureHandle,
                                    const ayt::math::FRectangle& uvRegion,
                                    const ayt::math::FVector4& padding)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    uint16_t textureIdx = detail::UiGpuContext::kInvalidIdx;
    uint16_t textureWidth = 0;
    uint16_t textureHeight = 0;
    if (!resolveTextureHandle(textureHandle, textureIdx, textureWidth, textureHeight)) {
        drawRect(bounds, kUnknownTextureColor);
        return;
    }

    // 9-slice layout is computed from the *unclipped* bounds — a clip
    // should crop the display, not re-layout the slices (the old code
    // intersected first, which also shrunk the corner-fit scale `s`).
    // Each slice is clipped + UV-remapped by emitClippedTexturedQuad.
    const float bW = bounds.maxX - bounds.minX;
    const float bH = bounds.maxY - bounds.minY;
    if (bW <= 0.0f || bH <= 0.0f) {
        return;
    }

    // padding = (left, top, right, bottom) in TEXTURE pixels, so corners
    // keep their natural size on screen (1:1 texel mapping). Bounds smaller
    // than the corner block: scale padding down to fit so the center slice
    // never collapses; UV padding follows the same scale.
    const float padL = std::max(0.0f, padding.x);
    const float padT = std::max(0.0f, padding.y);
    const float padR = std::max(0.0f, padding.z);
    const float padB = std::max(0.0f, padding.w);
    const float padSumX = padL + padR;
    const float padSumY = padT + padB;
    if (padSumX <= 0.0f || padSumY <= 0.0f) {
        // No corner area at all — a single stretched quad.
        emitClippedTexturedQuad(bounds, textureIdx, uvRegion,
                                ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
        return;
    }
    const float s  = std::min(1.0f, std::min(bW / padSumX, bH / padSumY));
    const float pl = padL * s, pr = padR * s, pt = padT * s, pb = padB * s;

    const float texW = static_cast<float>(textureWidth);
    const float texH = static_cast<float>(textureHeight);

    const float uSpan = std::max(0.0f, uvRegion.maxX - uvRegion.minX);
    const float vSpan = std::max(0.0f, uvRegion.maxY - uvRegion.minY);
    if (uSpan <= 0.0f || vSpan <= 0.0f) {
        emitClippedTexturedQuad(bounds, textureIdx, uvRegion,
                                ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
        return;
    }

    // Texture-space paddings; clamped to the region span so the center UV
    // slice never inverts (degenerate tiny textures + huge pixel padding).
    float uPl = pl / texW, uPr = pr / texW;
    float uPt = pt / texH, uPb = pb / texH;
    if (uPl + uPr > uSpan) {
        const float su = uSpan / (uPl + uPr);
        uPl *= su;
        uPr *= su;
    }
    if (uPt + uPb > vSpan) {
        const float sv = vSpan / (uPt + uPb);
        uPt *= sv;
        uPb *= sv;
    }

    // 3x3 grid: 4 corners keep their size, edges stretch one axis, the
    // center stretches both. All 9 quads share (textureIdx, state) so they
    // form a single flush run; each is individually clipped + UV-remapped.
    const float x[4] = {bounds.minX, bounds.minX + pl, bounds.maxX - pr, bounds.maxX};
    const float y[4] = {bounds.minY, bounds.minY + pt, bounds.maxY - pb, bounds.maxY};
    const float u[4] = {uvRegion.minX, uvRegion.minX + uPl,
                        uvRegion.maxX - uPr, uvRegion.maxX};
    const float v[4] = {uvRegion.minY, uvRegion.minY + uPt,
                        uvRegion.maxY - uPb, uvRegion.maxY};

    const ayt::math::FVector4 white(1.0f, 1.0f, 1.0f, 1.0f);
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            emitClippedTexturedQuad(ayt::math::FRectangle(x[i], y[j], x[i + 1], y[j + 1]),
                                    textureIdx,
                                    ayt::math::FRectangle(u[i], v[j], u[i + 1], v[j + 1]),
                                    white);
        }
    }
}

void UIRenderBackend::flushColoredRects()
{
    if (_frame == nullptr) {
        return;
    }

    FrameState& frame = *_frame;
    if (frame.items.empty() || !_initialized || _gpu == nullptr || _adapter == nullptr
        || frame.activeWidth < 1 || frame.activeHeight < 1) {
        frame.items.clear();
        return;
    }

    const uint16_t whiteIdx  = _gpu->whiteTextureIdx();
    const float    fbW       = static_cast<float>(frame.activeWidth);
    const float    fbH       = static_cast<float>(frame.activeHeight);
    const float    uiScale   = getUiScale();
    const auto pxX = [&](float logicalX) { return (logicalX - frame.originX) * uiScale; };
    const auto pxY = [&](float logicalY) { return (logicalY - frame.originY) * uiScale; };

    // OrderedRuns is the original fallback. The optimized path builds a
    // logical order only; UiItems themselves stay in recording order so a
    // planner failure can fall back without reconstructing the command list.
    const std::vector<uint32_t>* batchOrder = nullptr;
    if (frame.batchMode == BatchMode::OverlapAware
        && buildOverlapAwareOrder(frame.items, frame.batchOrder,
                                  frame.batchNext, frame.batchBarriers)) {
        batchOrder = &frame.batchOrder;
    }
    const auto itemAt = [&](size_t logicalIndex) -> const UiItem& {
        return batchOrder != nullptr ? frame.items[(*batchOrder)[logicalIndex]]
                                     : frame.items[logicalIndex];
    };

    const auto setDrawStencil = [](uint8_t depth) {
        if (depth == 0u) {
            bgfx::setStencil(BGFX_STENCIL_NONE, BGFX_STENCIL_NONE);
            return;
        }
        const uint32_t stencil = BGFX_STENCIL_TEST_EQUAL
            | BGFX_STENCIL_FUNC_REF(depth)
            | BGFX_STENCIL_FUNC_RMASK(0xff)
            | BGFX_STENCIL_OP_FAIL_S_KEEP
            | BGFX_STENCIL_OP_FAIL_Z_KEEP
            | BGFX_STENCIL_OP_PASS_Z_KEEP;
        bgfx::setStencil(stencil, stencil);
    };

    const auto buildMeshVertices = [&](const UiTriangleMesh& mesh, uint32_t color) {
        frame.scratchVertices.clear();
        frame.scratchIndices = mesh.indices;
        frame.scratchVertices.reserve(mesh.positions.size());
        for (const auto& point : mesh.positions) {
            frame.scratchVertices.push_back({
                toNdcX(pxX(point.x), fbW), toNdcY(pxY(point.y), fbH), 0.0f,
                color, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f
            });
        }
    };

    const auto submitMesh = [&](const UiTriangleMesh& mesh, uint32_t color,
                                uint64_t state, uint8_t depth) {
        if (mesh.indices.empty() || mesh.positions.empty()) return;
        buildMeshVertices(mesh, color);
        setDrawStencil(depth);
        _gpu->submitColoredQuads(frame.activeViewId, *_adapter, state,
                                 frame.scratchVertices.data(),
                                 static_cast<uint32_t>(frame.scratchVertices.size()),
                                 sizeof(UiVertex), frame.scratchIndices.data(),
                                 static_cast<uint32_t>(frame.scratchIndices.size()));
        ++_drawCalls;
    };

    const auto submitStencilMesh = [&](const UiTriangleMesh& mesh,
                                       uint8_t reference, uint32_t passOp) {
        if (mesh.indices.empty() || mesh.positions.empty()) return;
        buildMeshVertices(mesh, 0xffffffffu);
        const uint32_t stencil = BGFX_STENCIL_TEST_EQUAL
            | BGFX_STENCIL_FUNC_REF(reference)
            | BGFX_STENCIL_FUNC_RMASK(0xff)
            | BGFX_STENCIL_OP_FAIL_S_KEEP
            | BGFX_STENCIL_OP_FAIL_Z_KEEP
            | passOp;
        bgfx::setStencil(stencil, stencil);
        // A zero render state disables color/depth writes. ShaderResource's
        // state==0 path preserves the state we explicitly install here.
        _adapter->setState(0);
        _gpu->submitColoredQuads(frame.activeViewId, *_adapter, 0,
                                 frame.scratchVertices.data(),
                                 static_cast<uint32_t>(frame.scratchVertices.size()),
                                 sizeof(UiVertex), frame.scratchIndices.data(),
                                 static_cast<uint32_t>(frame.scratchIndices.size()));
        ++_drawCalls;
    };

    const auto collectContours = [](const UiPathSnapshot& path,
                                    ayt::ui::PathWinding winding) {
        UiTriangleMesh mesh;
        for (const auto& contour : path.fillContours) {
            if (contour.winding == winding) appendMesh(mesh, contour.mesh);
        }
        return mesh;
    };

    const auto pushPathStencil = [&](const UiPathSnapshot& path, uint8_t baseDepth) {
        const UiTriangleMesh outer = collectContours(
            path, ayt::ui::PathWinding::CounterClockwise);
        const UiTriangleMesh holes = collectContours(
            path, ayt::ui::PathWinding::Clockwise);
        submitStencilMesh(outer, baseDepth, BGFX_STENCIL_OP_PASS_Z_INCRSAT);
        if (baseDepth < UINT8_MAX) {
            submitStencilMesh(holes, static_cast<uint8_t>(baseDepth + 1u),
                              BGFX_STENCIL_OP_PASS_Z_DECRSAT);
        }
    };

    const auto popPathStencil = [&](const UiPathSnapshot& path, uint8_t depth) {
        const UiTriangleMesh outer = collectContours(
            path, ayt::ui::PathWinding::CounterClockwise);
        submitStencilMesh(outer, depth, BGFX_STENCIL_OP_PASS_Z_DECRSAT);
    };

    // Flush a consecutive run in the selected logical order. In fallback
    // mode logical order is exactly the original array/painter order.
    auto flushFlatRun = [&](size_t begin, size_t end) {
        const UiItem& first = itemAt(begin);
        const size_t  count = end - begin;

        frame.scratchVertices.clear();
        frame.scratchIndices.clear();
        frame.scratchVertices.reserve(count * 4u);
        frame.scratchIndices.reserve(count * 6u);

        for (size_t i = begin; i < end; ++i) {
            const UiItem& it = itemAt(i);
            const uint32_t base = static_cast<uint32_t>(frame.scratchVertices.size());
            const float    z    = 0.0f;

            frame.scratchVertices.push_back({toNdcX(pxX(it.minX), fbW), toNdcY(pxY(it.minY), fbH), z,
                                             it.abgr[0], it.u0, it.v0,
                                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(pxX(it.maxX), fbW), toNdcY(pxY(it.minY), fbH), z,
                                             it.abgr[1], it.u1, it.v0,
                                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(pxX(it.maxX), fbW), toNdcY(pxY(it.maxY), fbH), z,
                                             it.abgr[2], it.u1, it.v1,
                                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(pxX(it.minX), fbW), toNdcY(pxY(it.maxY), fbH), z,
                                             it.abgr[3], it.u0, it.v1,
                                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});

            frame.scratchIndices.push_back(base + 0);
            frame.scratchIndices.push_back(base + 1);
            frame.scratchIndices.push_back(base + 2);
            frame.scratchIndices.push_back(base + 0);
            frame.scratchIndices.push_back(base + 2);
            frame.scratchIndices.push_back(base + 3);
        }

        // P1: state is uniform within a run (grouping key), pass first.state.
        setDrawStencil(first.stencilDepth);
        if (first.textureIdx == whiteIdx) {
            _gpu->submitColoredQuads(frame.activeViewId, *_adapter, first.state,
                                     frame.scratchVertices.data(),
                                     static_cast<uint32_t>(frame.scratchVertices.size()),
                                     sizeof(UiVertex), frame.scratchIndices.data(),
                                     static_cast<uint32_t>(frame.scratchIndices.size()));
        } else {
            _gpu->submitTexturedQuads(frame.activeViewId, *_adapter, first.state, first.textureIdx,
                                      frame.scratchVertices.data(),
                                      static_cast<uint32_t>(frame.scratchVertices.size()),
                                      sizeof(UiVertex), frame.scratchIndices.data(),
                                      static_cast<uint32_t>(frame.scratchIndices.size()));
        }
        ++_drawCalls;
    };

    auto flushMeshRun = [&](size_t begin, size_t end) {
        const UiItem& first = itemAt(begin);
        // Mesh color is per-item, so rebuild directly instead of using the
        // single-color submitMesh helper.
        frame.scratchVertices.clear();
        frame.scratchIndices.clear();
        for (size_t itemIndex = begin; itemIndex < end; ++itemIndex) {
            const UiItem& item = itemAt(itemIndex);
            if (item.mesh == nullptr) continue;
            const uint32_t base = static_cast<uint32_t>(frame.scratchVertices.size());
            for (const auto& point : item.mesh->positions) {
                frame.scratchVertices.push_back({
                    toNdcX(pxX(point.x), fbW), toNdcY(pxY(point.y), fbH), 0.0f,
                    item.abgr[0], 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 0.0f
                });
            }
            for (uint32_t index : item.mesh->indices) {
                frame.scratchIndices.push_back(base + index);
            }
        }
        if (frame.scratchIndices.empty()) return;
        setDrawStencil(first.stencilDepth);
        _gpu->submitColoredQuads(frame.activeViewId, *_adapter, first.state,
                                 frame.scratchVertices.data(),
                                 static_cast<uint32_t>(frame.scratchVertices.size()),
                                 sizeof(UiVertex), frame.scratchIndices.data(),
                                 static_cast<uint32_t>(frame.scratchIndices.size()));
        ++_drawCalls;
    };

    size_t i = 0;
    const size_t n = frame.items.size();
    while (i < n) {
        const UiItem& it = itemAt(i);
        if (it.kind == UiItemKind::PathClipPush) {
            if (it.path != nullptr) pushPathStencil(*it.path, it.stencilDepth);
            ++i;
            continue;
        }
        if (it.kind == UiItemKind::PathClipPop) {
            if (it.path != nullptr) popPathStencil(*it.path, it.stencilDepth);
            ++i;
            continue;
        }
        if (it.kind == UiItemKind::PathFill) {
            if (it.path != nullptr) {
                pushPathStencil(*it.path, it.stencilDepth);
                UiTriangleMesh quad;
                quad.positions = {
                    {it.minX, it.minY}, {it.maxX, it.minY},
                    {it.maxX, it.maxY}, {it.minX, it.maxY}
                };
                quad.indices = {0, 1, 2, 0, 2, 3};
                submitMesh(quad, it.abgr[0], it.state,
                           static_cast<uint8_t>(it.stencilDepth + 1u));
                popPathStencil(*it.path,
                               static_cast<uint8_t>(it.stencilDepth + 1u));
            }
            ++i;
            continue;
        }
        if (it.kind == UiItemKind::Mesh) {
            size_t end = i + 1u;
            while (end < n && itemAt(end).kind == UiItemKind::Mesh
                   && itemAt(end).state == it.state
                   && itemAt(end).stencilDepth == it.stencilDepth) {
                ++end;
            }
            flushMeshRun(i, end);
            i = end;
            continue;
        }
        if (it.kind == UiItemKind::Sdf) {
            // Batch knife: consecutive SDF items with IDENTICAL params
            // (shape rect rides per-vertex now, so position/color vary
            // freely within a run) and same blend state merge into one
            // submission. Per-item uniforms would force one call per
            // quad; same-skin buttons/panels collapse to a single call.
            // itemAt() already reflects either the overlap-safe logical
            // order or the untouched painter-order fallback.
            size_t end = i + 1;
            while (end < n && itemAt(end).kind == UiItemKind::Sdf
                   && itemAt(end).state == it.state
                   && itemAt(end).stencilDepth == it.stencilDepth
                   && sdfParamsEqual(itemAt(end).sdf, it.sdf)) {
                ++end;
            }

            frame.scratchVertices.clear();
            frame.scratchIndices.clear();
            frame.scratchVertices.reserve((end - i) * 4u);
            frame.scratchIndices.reserve((end - i) * 6u);
            const float z = 0.0f;

            for (size_t k = i; k < end; ++k) {
                const UiItem& s = itemAt(k);
                const uint32_t base = static_cast<uint32_t>(frame.scratchVertices.size());
                // Fill from abgr[0] (drawRoundedRect); borders leave it 0.
                const uint32_t fill = s.abgr[0];
                const float cx = pxX((s.shapeMinX + s.shapeMaxX) * 0.5f);
                const float cy = pxY((s.shapeMinY + s.shapeMaxY) * 0.5f);
                const float hw = (s.shapeMaxX - s.shapeMinX) * 0.5f * uiScale;
                const float hh = (s.shapeMaxY - s.shapeMinY) * 0.5f * uiScale;
                // Soft-clip rect as (center, half-extent); all-zero
                // (chw == 0) = no clip, shader skips the seam fade.
                float ccx = 0.0f, ccy = 0.0f, chw = 0.0f, chh = 0.0f;
                if (s.clipMaxX > s.clipMinX && s.clipMaxY > s.clipMinY) {
                    ccx = pxX((s.clipMinX + s.clipMaxX) * 0.5f);
                    ccy = pxY((s.clipMinY + s.clipMaxY) * 0.5f);
                    chw = (s.clipMaxX - s.clipMinX) * 0.5f * uiScale;
                    chh = (s.clipMaxY - s.clipMinY) * 0.5f * uiScale;
                }
                // a_texcoord0 / v_pos MUST be draw-quad pixel corners
                // (minX..maxY), not shapeMin/Max. Shape is often inset
                // (outside stroke / shadow blur expand the draw quad);
                // packing shape corners warps SDF space so stroke rings
                // collapse to corner arcs and soft shadows become solid
                // blocks. Shape silhouette stays in TexCoord1 (cx,cy,hw,hh).
                frame.scratchVertices.push_back({toNdcX(pxX(s.minX), fbW), toNdcY(pxY(s.minY), fbH), z,
                                                 fill, pxX(s.minX), pxY(s.minY), cx, cy, hw, hh,
                                                 ccx, ccy, chw, chh});
                frame.scratchVertices.push_back({toNdcX(pxX(s.maxX), fbW), toNdcY(pxY(s.minY), fbH), z,
                                                 fill, pxX(s.maxX), pxY(s.minY), cx, cy, hw, hh,
                                                 ccx, ccy, chw, chh});
                frame.scratchVertices.push_back({toNdcX(pxX(s.maxX), fbW), toNdcY(pxY(s.maxY), fbH), z,
                                                 fill, pxX(s.maxX), pxY(s.maxY), cx, cy, hw, hh,
                                                 ccx, ccy, chw, chh});
                frame.scratchVertices.push_back({toNdcX(pxX(s.minX), fbW), toNdcY(pxY(s.maxY), fbH), z,
                                                 fill, pxX(s.minX), pxY(s.maxY), cx, cy, hw, hh,
                                                 ccx, ccy, chw, chh});
                frame.scratchIndices.push_back(base + 0);
                frame.scratchIndices.push_back(base + 1);
                frame.scratchIndices.push_back(base + 2);
                frame.scratchIndices.push_back(base + 0);
                frame.scratchIndices.push_back(base + 2);
                frame.scratchIndices.push_back(base + 3);
            }

            setDrawStencil(it.stencilDepth);
            detail::UiGpuContext::SdfParams scaledSdf = it.sdf;
            scaledSdf.radius = ayt::math::FVector4(
                it.sdf.radius.x * uiScale, it.sdf.radius.y * uiScale,
                it.sdf.radius.z * uiScale, it.sdf.radius.w * uiScale);
            scaledSdf.strokeWidth *= uiScale;
            scaledSdf.strokeInset *= uiScale;
            scaledSdf.shadowOffset = ayt::math::FVector2(
                it.sdf.shadowOffset.x * uiScale,
                it.sdf.shadowOffset.y * uiScale);
            scaledSdf.shadowBlur *= uiScale;
            _gpu->submitSdfQuads(frame.activeViewId, *_adapter, it.state,
                                 frame.scratchVertices.data(),
                                 static_cast<uint32_t>(frame.scratchVertices.size()),
                                 sizeof(UiVertex), frame.scratchIndices.data(),
                                 static_cast<uint32_t>(frame.scratchIndices.size()),
                                 scaledSdf);
            ++_drawCalls;
            i = end;
            continue;
        }
        size_t end = i + 1;
        while (end < n && itemAt(end).kind == UiItemKind::Flat
               && itemAt(end).textureIdx == it.textureIdx
               && itemAt(end).state == it.state
               && itemAt(end).stencilDepth == it.stencilDepth) {
            ++end;
        }
        flushFlatRun(i, end);
        i = end;
    }

    frame.items.clear();
    bgfx::setStencil(BGFX_STENCIL_NONE, BGFX_STENCIL_NONE);
}

void UIRenderBackend::flushPendingText()
{
    // P0: text glyphs are UiItems in the unified batch; flushColoredRects
    // (called from endFrame / flushBatches) submits them. Kept as a
    // declared no-op so endFrame's call sequence stays unchanged.
}

void UIRenderBackend::drawTexturedQuad(const ayt::math::FRectangle& bounds, uint16_t textureIdx,
                                       const ayt::math::FVector4& tint)
{
    if (!_initialized || _gpu == nullptr || _adapter == nullptr || _frame == nullptr
        || _frame->activeWidth < 1 || _frame->activeHeight < 1) {
        return;
    }

    const uint32_t abgr = toAbgr(tint);
    const float scale = getUiScale();
    const float fbW = static_cast<float>(_frame->activeWidth);
    const float fbH = static_cast<float>(_frame->activeHeight);
    const auto pxX = [&](float x) { return (x - _frame->originX) * scale; };
    const auto pxY = [&](float y) { return (y - _frame->originY) * scale; };
    const UiVertex vertices[4] = {
        {toNdcX(pxX(bounds.minX), fbW), toNdcY(pxY(bounds.minY), fbH),
         0.0f, abgr, 0.0f, 0.0f,
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {toNdcX(pxX(bounds.maxX), fbW), toNdcY(pxY(bounds.minY), fbH),
         0.0f, abgr, 1.0f, 0.0f,
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {toNdcX(pxX(bounds.maxX), fbW), toNdcY(pxY(bounds.maxY), fbH),
         0.0f, abgr, 1.0f, 1.0f,
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {toNdcX(pxX(bounds.minX), fbW), toNdcY(pxY(bounds.maxY), fbH),
         0.0f, abgr, 0.0f, 1.0f,
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    };
    const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};

    _gpu->submitTexturedQuads(_frame->activeViewId, *_adapter,
                              blendStateBits(ayt::ui::BlendMode::Normal),
                              textureIdx, vertices, 4, sizeof(UiVertex), indices, 6);
}

ayt::ui::IRenderBackend::TextMetrics UIRenderBackend::measureText(const std::wstring& text,
                                                                  int fontSize,
                                                                  float maxWidth) const
{
    ayt::ui::IRenderBackend::TextStyle style;
    return measureText(text, fontSize, style, maxWidth);
}

ayt::ui::IRenderBackend::TextMetrics UIRenderBackend::measureText(
    const std::wstring& text, int fontSize,
    const ayt::ui::IRenderBackend::TextStyle& style, float maxWidth) const
{
    TextMetrics out{0.0f, 0.0f, 0.0f, 0.0f};
    if (!_initialized || _fontAtlas == nullptr || fontSize < 8) {
        return out;
    }

    auto* atlas = const_cast<detail::BgfxFontAtlas*>(_fontAtlas.get());
    const float scale = getUiScale();
    const int rasterSize = std::max(1, static_cast<int>(std::lround(fontSize * scale)));
    const wchar_t* family = style.fontFamily.empty() ? nullptr : style.fontFamily.c_str();
    const int weight = style.bold ? std::max(700, style.fontWeight) : style.fontWeight;
    ayt::font::IFont* font = atlas->acquireFont(family, rasterSize, weight, style.italic);
    if (font == nullptr) {
        return out;
    }

    const ayt::font::FontMetrics& fm = font->getMetrics();
    out.ascent  = fm.ascent / scale;
    out.descent = (fm.descent > 0.0f ? fm.descent : -fm.descent) / scale;
    out.height  = (fm.lineHeight > 0.0f ? fm.lineHeight
                                        : static_cast<float>(rasterSize)) / scale;

    if (text.empty()) {
        return out;
    }

    // Glyph advances (px). Shaped path = HB 26.6 fixed-point; fallback =
    // per-codepoint advance (same default advance the draw path uses).
    const char* language = style.language.empty() ? nullptr : style.language.c_str();
    const auto shapedRuns = atlas->shapeTextWithFallback(
        font, rasterSize, text, toFontDirection(style.direction), language);
    std::vector<ayt::font::ShapedGlyph> shaped;
    for (const auto& run : shapedRuns) {
        shaped.insert(shaped.end(), run.glyphs.begin(), run.glyphs.end());
    }
    std::vector<float> advances;
    std::vector<bool>  isSpace;
    if (!shaped.empty()) {
        advances.reserve(shaped.size());
        isSpace.reserve(shaped.size());
        for (size_t begin = 0; begin < shaped.size();) {
            size_t end = begin;
            float advance = 0.0f;
            do {
                advance += static_cast<float>(shaped[end].xAdvance) / (64.0f * scale);
                ++end;
            } while (end < shaped.size()
                     && shaped[end].charIndex == shaped[begin].charIndex);
            advances.push_back(advance + static_cast<float>(style.letterSpacing));
            const size_t ci = std::min<size_t>(shaped[begin].charIndex, text.size() - 1u);
            isSpace.push_back(text[ci] == L' ' || text[ci] == L'\t');
            begin = end;
        }
    } else {
        const auto analysis = ayt::ui::analyzeUnicodeText(text, style.direction);
        advances.reserve(analysis.clusters.size());
        isSpace.reserve(analysis.clusters.size());
        for (const auto& cluster : analysis.clusters) {
            float advance = 0.0f;
            size_t index = cluster.textStart;
            const size_t end = cluster.textStart + cluster.textLength;
            while (index < end) {
                ayt::font::GlyphInfo* glyph = font->getGlyph(nextTextCodePoint(text, index));
                advance += (glyph != nullptr)
                    ? static_cast<float>(glyph->metrics.advance) / scale
                    : out.height * 0.25f;
            }
            advances.push_back(advance + static_cast<float>(style.letterSpacing));
            isSpace.push_back(cluster.whitespace);
        }
    }
    const size_t n = advances.size();

    if (maxWidth <= 0.0f) {
        // No wrap constraint: single line, full span.
        for (float a : advances) {
            out.width += a;
        }
        return out;
    }

    // Greedy word wrap (maxWidth px): break at the last space that fits;
    // a word wider than maxWidth hard-breaks per glyph (CJK has no
    // spaces, so it degrades to per-glyph breaking naturally).
    const float lh = out.height > 0.0f ? out.height : static_cast<float>(fontSize);
    std::vector<float> prefix(n + 1, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + advances[i];
    }

    int    lines     = 1;
    float  lineW     = 0.0f;
    float  maxLineW  = 0.0f;
    size_t lineStart = 0;
    size_t lastBreak = n;  // index of the last space that fit in the line; n = none
    for (size_t i = 0; i < n; ++i) {
        const float w = advances[i];
        if (lineW + w <= maxWidth || lineW <= 0.0f) {
            lineW += w;
            if (isSpace[i]) {
                lastBreak = i;
            }
        } else {
            // Line full. Break after the last fitting space when one is in
            // this line; the space itself stays on the old line, so the new
            // line's width = glyphs (lastBreak+1 .. i]. Else hard-break.
            if (lastBreak != n && lastBreak >= lineStart) {
                lineW = prefix[i + 1] - prefix[lastBreak + 1];
                lineStart = lastBreak + 1;
                lastBreak = n;
            } else {
                lineW     = w;
                lineStart = i;
            }
            if (isSpace[i]) {
                lastBreak = i;
            }
            ++lines;
        }
        maxLineW = std::max(maxLineW, lineW);
    }

    out.width  = maxLineW;
    out.height = static_cast<float>(lines) * lh;
    return out;
}

ayt::ui::IRenderBackend::ShapedText UIRenderBackend::shapeText(
    const std::wstring& text, int fontSize,
    const ayt::ui::IRenderBackend::TextStyle& style) const
{
    ShapedText out;
    out.metrics = measureText(text, fontSize, style);
    if (!_initialized || _fontAtlas == nullptr || text.empty() || fontSize < 8) {
        return out;
    }

    auto* atlas = const_cast<detail::BgfxFontAtlas*>(_fontAtlas.get());
    const float scale = getUiScale();
    const int rasterSize = std::max(1, static_cast<int>(std::lround(fontSize * scale)));
    const wchar_t* family = style.fontFamily.empty() ? nullptr : style.fontFamily.c_str();
    const int weight = style.bold ? std::max(700, style.fontWeight) : style.fontWeight;
    ayt::font::IFont* font = atlas->acquireFont(family, rasterSize, weight, style.italic);
    if (font == nullptr) {
        return ayt::ui::IRenderBackend::shapeText(text, fontSize, style);
    }

    const char* language = style.language.empty() ? nullptr : style.language.c_str();
    const auto shapedRuns = atlas->shapeTextWithFallback(
        font, rasterSize, text, toFontDirection(style.direction), language);
    std::vector<ayt::font::ShapedGlyph> shaped;
    for (const auto& run : shapedRuns) {
        shaped.insert(shaped.end(), run.glyphs.begin(), run.glyphs.end());
    }
    if (shaped.empty()) {
        return ayt::ui::IRenderBackend::shapeText(text, fontSize, style);
    }

    const ayt::ui::UnicodeTextAnalysis analysis =
        ayt::ui::analyzeUnicodeText(text, style.direction);
    out.rightToLeft = style.direction == ayt::ui::TextDirection::RightToLeft
        || (style.direction == ayt::ui::TextDirection::Auto && analysis.baseRightToLeft);

    std::vector<size_t> sourceStarts;
    sourceStarts.reserve(shaped.size());
    const size_t lastSourceIndex = text.size() - 1u;
    for (const ayt::font::ShapedGlyph& glyph : shaped) {
        sourceStarts.push_back(std::min<size_t>(glyph.charIndex, lastSourceIndex));
    }
    std::sort(sourceStarts.begin(), sourceStarts.end());
    sourceStarts.erase(std::unique(sourceStarts.begin(), sourceStarts.end()),
                       sourceStarts.end());

    auto sourceLength = [&](size_t start) {
        const auto next = std::upper_bound(sourceStarts.begin(), sourceStarts.end(), start);
        const size_t end = next == sourceStarts.end() ? text.size() : *next;
        return end > start ? end - start : size_t{1};
    };

    const float letterSpacing = static_cast<float>(style.letterSpacing);
    float pen = 0.0f;
    for (size_t begin = 0; begin < shaped.size();) {
        const size_t start = std::min<size_t>(shaped[begin].charIndex, lastSourceIndex);
        size_t end = begin + 1u;
        float advance = static_cast<float>(shaped[begin].xAdvance) / (64.0f * scale);
        while (end < shaped.size() && shaped[end].charIndex == shaped[begin].charIndex) {
            advance += static_cast<float>(shaped[end].xAdvance) / (64.0f * scale);
            ++end;
        }
        advance += letterSpacing;
        const float nextPen = pen + advance;
        out.clusters.push_back({start, sourceLength(start),
                                std::min(pen, nextPen), std::max(pen, nextPen),
                                static_cast<uint8_t>(out.rightToLeft ? 1u : 0u)});
        pen = nextPen;
        begin = end;
    }
    out.metrics.width = std::abs(pen);
    return out;
}

ayt::font::FontMetrics UIRenderBackend::getFontMetrics(ayt::font::FontHandle font) const
{
    if (!_initialized || _fontAtlas == nullptr) {
        return ayt::font::FontMetrics{0, 0, 0, 0, 0, 0};
    }
    ayt::font::IFont* face = _fontAtlas->fontForHandle(font);
    if (face == nullptr) {
        return ayt::font::FontMetrics{0, 0, 0, 0, 0, 0};
    }
    return face->getMetrics();
}

ayt::font::FontHandle UIRenderBackend::getFontHandle(const wchar_t* familyName, int baseSize)
{
    if (!_initialized || _fontAtlas == nullptr) {
        return ayt::font::FontHandle{-1};
    }
    // Family-aware: known family names resolve to their registered face
    // (BgfxFontAtlas seeds the family→file table from the default system
    // candidates); unknown or empty names fall back to the size-keyed
    // default face — the legacy behavior.
    const int rasterSize = std::max(
        1, static_cast<int>(std::lround(baseSize * getUiScale())));
    if (familyName != nullptr && familyName[0] != L'\0') {
        ayt::font::IFont* familyFont = _fontAtlas->acquireFont(familyName, rasterSize);
        if (familyFont != nullptr) {
            return familyFont->getHandle();
        }
    }
    if (_fontAtlas->acquireFont(rasterSize) == nullptr) {
        return ayt::font::FontHandle{-1};
    }
    return _fontAtlas->handleForSize(rasterSize);
}

void UIRenderBackend::drawText(const ayt::math::FRectangle& bounds, const std::wstring& text,
                               int fontSize, const ayt::math::FVector4& color)
{
    // Style route: default style keeps the legacy single-line behavior
    // (Align::Left + VAlign::Middle == the old centered baseline).
    ayt::ui::IRenderBackend::TextStyle style;
    style.color = color;
    drawText(bounds, text, fontSize, style);
}

void UIRenderBackend::drawText(const ayt::math::FRectangle& bounds, const std::wstring& text,
                               int fontSize, const ayt::ui::IRenderBackend::TextStyle& style)
{
    if (!_initialized || text.empty() || _width < 1 || _height < 1 || _gpu == nullptr
        || _adapter == nullptr || _fontAtlas == nullptr) {
        return;
    }

    // This implementation honors color / outline / shadow / letterSpacing /
    // lineSpacing / align / valign. wrapToBounds enables multi-line layout
    // (greedy word wrap to the bounds width); without it text stays
    // single-line and clips at the bounds — the legacy behavior.

    ayt::math::FRectangle clippedBounds = bounds;
    if (!clipRect(clippedBounds)) {
        return;
    }

    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    FrameState& frame = *_frame;

    const float scale = getUiScale();
    const int rasterSize = std::max(1, static_cast<int>(std::lround(fontSize * scale)));
    const wchar_t* family = style.fontFamily.empty() ? nullptr : style.fontFamily.c_str();
    const int weight = style.bold ? std::max(700, style.fontWeight) : style.fontWeight;
    ayt::font::IFont* font = _fontAtlas->acquireFont(
        family, rasterSize, weight, style.italic);
    if (font == nullptr) {
        drawRect(bounds, style.color);
        return;
    }

    const char* language = style.language.empty() ? nullptr : style.language.c_str();
    const auto shapedFontRuns = _fontAtlas->shapeTextWithFallback(
        font, rasterSize, text, toFontDirection(style.direction), language);
    std::vector<ayt::font::ShapedGlyph> shaped;
    for (const auto& run : shapedFontRuns) {
        shaped.insert(shaped.end(), run.glyphs.begin(), run.glyphs.end());
    }
    const bool useShaped = !shapedFontRuns.empty();
    if (useShaped) {
        for (const auto& run : shapedFontRuns) {
            _fontAtlas->prepareShapedGlyphs(run.font, rasterSize, run.glyphs);
            syncTextAtlasIfNeeded(run.font);
        }
    } else {
        _fontAtlas->prepareGlyphs(font, rasterSize, text);
        syncTextAtlasIfNeeded(font);
    }

    const ayt::font::FontMetrics& metrics = font->getMetrics();
    const float boundsW = bounds.maxX - bounds.minX;
    const float boundsH = bounds.maxY - bounds.minY;

    // Per-glyph run data: glyph + HB offsets + advance (px). One array
    // feeds measuring, wrapping and emitting, so line widths and pen
    // positions can never diverge.
    struct GlyphRun {
        ayt::font::GlyphInfo* glyph;
        uint16_t atlasIdx;
        float xOff;
        float yOff;
        float xAdv;
        bool  space;
        bool  clusterEnd;
    };
    std::vector<GlyphRun> runs;
    if (useShaped) {
        runs.reserve(shaped.size());
        for (const auto& fontRun : shapedFontRuns) {
            const uint16_t runAtlas = _fontAtlas->atlasTextureIdx(fontRun.font);
            for (size_t index = 0; index < fontRun.glyphs.size(); ++index) {
                const ayt::font::ShapedGlyph& sg = fontRun.glyphs[index];
                const size_t ci = std::min<size_t>(sg.charIndex, text.size() - 1);
                runs.push_back({fontRun.font->getGlyphByIndex(sg.glyphIndex), runAtlas,
                                static_cast<float>(sg.xOffset) / (64.0f * scale),
                                static_cast<float>(sg.yOffset) / (64.0f * scale),
                                static_cast<float>(sg.xAdvance) / (64.0f * scale),
                                text[ci] == L' ' || text[ci] == L'\t',
                                index + 1u == fontRun.glyphs.size()
                                    || fontRun.glyphs[index + 1u].charIndex != sg.charIndex});
            }
        }
    } else {
        const auto analysis = ayt::ui::analyzeUnicodeText(text, style.direction);
        runs.reserve(text.size());
        for (const auto& cluster : analysis.clusters) {
            size_t index = cluster.textStart;
            const size_t end = cluster.textStart + cluster.textLength;
            while (index < end) {
                ayt::font::GlyphInfo* glyph = font->getGlyph(nextTextCodePoint(text, index));
                runs.push_back({glyph, _fontAtlas->atlasTextureIdx(font), 0.0f, 0.0f,
                                (glyph != nullptr)
                                    ? static_cast<float>(glyph->metrics.advance) / scale
                                    : (metrics.lineHeight / scale) * 0.25f,
                                cluster.whitespace, index == end});
            }
        }
    }
    const size_t n = runs.size();
    if (n == 0) {
        return;
    }

    const float ls = static_cast<float>(style.letterSpacing);

    // Wrap shaping clusters, never individual glyphs: a ligature, emoji ZWJ
    // sequence or Arabic joining cluster is indivisible. Letter spacing is
    // also applied once per cluster, matching shapeText()/measureText().
    struct TextClusterRun {
        int begin = 0;
        int end = 0;
        float width = 0.0f;
        bool space = false;
    };
    std::vector<TextClusterRun> clusters;
    for (size_t begin = 0; begin < n;) {
        size_t end = begin;
        float width = 0.0f;
        do {
            width += runs[end].xAdv;
            ++end;
        } while (end < n && !runs[end - 1u].clusterEnd);
        width += ls;
        clusters.push_back({static_cast<int>(begin), static_cast<int>(end),
                            width, runs[begin].space});
        begin = end;
    }
    std::vector<float> clusterAdvances;
    std::vector<bool> clusterSpaces;
    clusterAdvances.reserve(clusters.size());
    clusterSpaces.reserve(clusters.size());
    for (const TextClusterRun& cluster : clusters) {
        clusterAdvances.push_back(cluster.width);
        clusterSpaces.push_back(cluster.space);
    }
    std::vector<UiTextLineRange> lines;
    if (style.wrapToBounds && boundsW > 0.0f
        && uiTextLineWidth(clusterAdvances.data(), static_cast<int>(clusters.size()), 0.0f)
            > boundsW) {
        std::vector<UiTextLineRange> clusterLines;
        uiTextWrapToLines(clusterAdvances.data(), clusterSpaces,
                          static_cast<int>(clusters.size()), boundsW, 0.0f,
                          clusterLines);
        lines.reserve(clusterLines.size());
        for (const UiTextLineRange& line : clusterLines) {
            lines.push_back({clusters[static_cast<size_t>(line.begin)].begin,
                             clusters[static_cast<size_t>(line.end - 1)].end,
                             line.width});
        }
    } else {
        lines.push_back({0, static_cast<int>(n),
                         uiTextLineWidth(clusterAdvances.data(),
                                         static_cast<int>(clusters.size()), 0.0f)});
    }

    // Block layout: lines sit at lineHeight + lineSpacing stride; VAlign
    // positions the whole block (blockH > boundsH clamps top-aligned).
    const float lineHeight =
        metrics.lineHeight > 0.0f ? metrics.lineHeight / scale
                                  : static_cast<float>(fontSize);
    const float stride = lineHeight + static_cast<float>(style.lineSpacing);
    const float blockH = lineHeight + stride * (static_cast<float>(lines.size()) - 1.0f);
    const float firstBaseline =
        uiTextBaselineY(style.valign, bounds.minY, boundsH, blockH,
                        metrics.ascent / scale);

    // Render passes back-to-front: shadow (offset copy) → outline (4-dir
    // offset copies) → fill. All passes emit the same atlas/state items,
    // so they merge into a single flush run. The shadow is a flat tinted
    // copy — shadowBlurRadius is reserved for a future text-shader blur
    // (atlas quads are bitmaps; per-item gaussian is not in this pass).
    struct Pass {
        ayt::math::FVector4 color;
        float dx;
        float dy;
    };
    std::vector<Pass> passes;
    if (style.shadowColor.w > 0.0f) {
        passes.push_back({style.shadowColor, style.shadowOffset.x, style.shadowOffset.y});
    }
    if (style.outlineWidth > 0.0f && style.outlineColor.w > 0.0f) {
        const float ow = style.outlineWidth;
        passes.push_back({style.outlineColor, -ow, 0.0f});
        passes.push_back({style.outlineColor, +ow, 0.0f});
        passes.push_back({style.outlineColor, 0.0f, -ow});
        passes.push_back({style.outlineColor, 0.0f, +ow});
    }
    passes.push_back({style.color, 0.0f, 0.0f});

    const float atlasW = static_cast<float>(detail::BgfxFontAtlas::kAtlasWidth);
    const float atlasH = static_cast<float>(detail::BgfxFontAtlas::kAtlasHeight);

    auto emitGlyph = [&](const GlyphRun& g, float penX, float penY, uint32_t colorAbgr) {
        if (g.glyph == nullptr) {
            return;
        }

        const int glyphW = g.glyph->metrics.width;
        const int glyphH = g.glyph->metrics.height;
        if (glyphW <= 0 || glyphH <= 0) {
            return;
        }

        const float unsnappedX0 = penX + g.xOff
                                + static_cast<float>(g.glyph->metrics.bearingX) / scale;
        // HB y_offset is up-positive; screen Y grows downward.
        const float unsnappedY0 = penY - g.yOff
                                - static_cast<float>(g.glyph->metrics.bearingY) / scale;
        // Glyph bitmaps are point sampled.  Keep their quad edges on the
        // physical pixel grid so a default framebuffer and an offscreen FBO
        // cannot choose opposite coverage for the same half-pixel boundary.
        // The pen and HarfBuzz advances remain fractional; only each bitmap's
        // final placement is snapped, so kerning does not accumulate error.
        const float x0 = std::round(unsnappedX0 * scale) / scale;
        const float y0 = std::round(unsnappedY0 * scale) / scale;
        const float x1 = x0 + static_cast<float>(glyphW) / scale;
        const float y1 = y0 + static_cast<float>(glyphH) / scale;

        ayt::math::FRectangle glyphBounds(x0, y0, x1, y1);
        if (!clipRect(glyphBounds)) {
            return;
        }

        const float u0 = g.glyph->atlasPosX / atlasW;
        const float v0 = g.glyph->atlasPosY / atlasH;
        const float u1 = (g.glyph->atlasPosX + g.glyph->atlasWidth) / atlasW;
        const float v1 = (g.glyph->atlasPosY + g.glyph->atlasHeight) / atlasH;

        // Remap UVs when the quad is partially clipped so boundary glyphs
        // stay visible (cropped), instead of the old "drop if not fully
        // inside" path that made rows pop in/out at ScrollView edges.
        float tu0 = u0, tv0 = v0, tu1 = u1, tv1 = v1;
        const float gw = x1 - x0;
        const float gh = y1 - y0;
        if (gw > 1e-5f && gh > 1e-5f) {
            tu0 = u0 + (u1 - u0) * ((glyphBounds.minX - x0) / gw);
            tv0 = v0 + (v1 - v0) * ((glyphBounds.minY - y0) / gh);
            tu1 = u0 + (u1 - u0) * ((glyphBounds.maxX - x0) / gw);
            tv1 = v0 + (v1 - v0) * ((glyphBounds.maxY - y0) / gh);
        }

        // P0 unified batch: one UiItem per glyph quad; all glyphs of a
        // drawText call share (atlas, state) so they form one flush run.
        UiItem item;
        item.textureIdx = g.atlasIdx;
        item.state = blendStateBits(frame.currentBlend);  // P1
        item.minX = glyphBounds.minX;
        item.minY = glyphBounds.minY;
        item.maxX = glyphBounds.maxX;
        item.maxY = glyphBounds.maxY;
        item.abgr[0] = colorAbgr;
        item.abgr[1] = colorAbgr;
        item.abgr[2] = colorAbgr;
        item.abgr[3] = colorAbgr;
        item.u0 = tu0;
        item.v0 = tv0;
        item.u1 = tu1;
        item.v1 = tv1;
        item.stencilDepth = frame.pathClipDepth;
        frame.items.push_back(item);
    };

    // PR-anim: per-pass color fades with the tree.
    for (const Pass& pass : passes) {
        ayt::math::FVector4 passColor = pass.color;
        passColor.w *= frame.opacityStack.back();
        const uint32_t abgr = toAbgr(passColor);
        for (size_t li = 0; li < lines.size(); ++li) {
            const UiTextLineRange& line = lines[li];
            const float cursorX = uiTextAlignX(style.align, bounds.minX, boundsW, line.width);
            const float baselineY = firstBaseline + stride * static_cast<float>(li);
            float penX = cursorX + pass.dx;
            const float penY = baselineY + pass.dy;
            for (int gi = line.begin; gi < line.end; ++gi) {
                const GlyphRun& g = runs[static_cast<size_t>(gi)];
                const bool fillPass = &pass == &passes.back();
                if (g.glyph != nullptr && g.glyph->colorBitmap && !fillPass) {
                    penX += g.xAdv + (g.clusterEnd ? ls : 0.0f);
                    continue;
                }
                uint32_t glyphColor = abgr;
                if (g.glyph != nullptr && g.glyph->colorBitmap
                    && fillPass) {
                    ayt::math::FVector4 intrinsic(1.0f, 1.0f, 1.0f, passColor.w);
                    glyphColor = toAbgr(intrinsic);
                }
                emitGlyph(g, penX, penY, glyphColor);
                penX += g.xAdv + (g.clusterEnd ? ls : 0.0f);
            }
        }
    }
}

} // namespace ayt::render
