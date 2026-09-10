#include "detail/EditorOverlayPass.h"
#include "AYShader/ShaderResource.h"

#include "AYMath/MathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ayt::render::detail
{

namespace {

struct AxisVertex {
    float x;
    float y;
    float z;
    // Applied in homogeneous clip space after projecting x/y/z. Arrow
    // vertices use zero; label vertices use this to stay screen-facing.
    float screenOffsetX;
    float screenOffsetY;
    uint32_t abgr;
};

static_assert(sizeof(AxisVertex) == 24,
              "orientation-axis vertex layout must stay Position3+Offset2+ABGR8");

constexpr const char* kAxisVaryingDef = R"(
vec4 v_color0   : COLOR0 = vec4(1.0, 1.0, 1.0, 1.0);
vec3 a_position : POSITION;
vec2 a_texcoord0: TEXCOORD0;
vec4 a_color0   : COLOR0;
)";

constexpr const char* kAxisVertexShader = R"(
$input a_position, a_texcoord0, a_color0
$output v_color0
#include <bgfx_shader.sh>
void main()
{
    vec4 clipPosition = mul(u_modelViewProj, vec4(a_position, 1.0));
    // Offsets are expressed in widget NDC. Multiplying by W keeps the
    // programmatic X/Y/Z glyphs upright and constant-sized on screen while
    // their 3D anchors continue to follow the projected arrow endpoints.
    clipPosition.xy += a_texcoord0 * clipPosition.w;
    gl_Position = clipPosition;
    v_color0 = a_color0;
}
)";

constexpr const char* kAxisFragmentShader = R"(
$input v_color0
#include <bgfx_shader.sh>
void main()
{
    gl_FragColor = v_color0;
}
)";

constexpr const char* kAxisShaderCacheKey =
    "editor_orientation_axis_labeled_v2";

constexpr uint32_t kGizmoX = 0xff4f4fe8u;
constexpr uint32_t kGizmoY = 0xff67c956u;
constexpr uint32_t kGizmoZ = 0xffff8d4du;
constexpr uint32_t kGizmoXY = 0xff4fa6c7u;
constexpr uint32_t kGizmoYZ = 0xffadad55u;
constexpr uint32_t kGizmoZX = 0xffa05ba9u;
constexpr uint32_t kGizmoUniform = 0xffd7d7d7u;
constexpr uint32_t kGizmoHighlight = 0xff4fe8ffu;

uint32_t gizmoColor(uint32_t normal,
                    uint8_t handle,
                    uint8_t highlighted,
                    uint16_t disabledHandles) noexcept
{
    if ((disabledHandles & static_cast<uint16_t>(1u << handle)) != 0u) {
        // Overlay rendering is opaque, so dim RGB rather than alpha. Keeping
        // the hue makes the unavailable direction legible without suggesting
        // that it can currently be dragged.
        constexpr uint32_t scale = 36u;
        const uint32_t r = ((normal      ) & 0xffu) * scale / 100u;
        const uint32_t g = ((normal >>  8) & 0xffu) * scale / 100u;
        const uint32_t b = ((normal >> 16) & 0xffu) * scale / 100u;
        return (normal & 0xff000000u) | (b << 16) | (g << 8) | r;
    }
    return handle == highlighted ? kGizmoHighlight : normal;
}

uint16_t appendVertex(std::vector<AxisVertex>& vertices,
                      const ayt::math::FVector3& p,
                      uint32_t abgr,
                      float screenOffsetX = 0.0f,
                      float screenOffsetY = 0.0f)
{
    const uint16_t index = static_cast<uint16_t>(vertices.size());
    vertices.push_back(AxisVertex{
        p.x, p.y, p.z, screenOffsetX, screenOffsetY, abgr});
    return index;
}

void appendLabelStroke(std::vector<AxisVertex>& vertices,
                       std::vector<uint16_t>& indices,
                       const ayt::math::FVector3& anchor,
                       float x0, float y0, float x1, float y1,
                       float thickness,
                       uint32_t abgr)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::max(0.0001f, std::sqrt(dx * dx + dy * dy));
    const float px = -dy / length * thickness * 0.5f;
    const float py =  dx / length * thickness * 0.5f;
    const uint16_t base = static_cast<uint16_t>(vertices.size());
    appendVertex(vertices, anchor, abgr, x0 + px, y0 + py);
    appendVertex(vertices, anchor, abgr, x0 - px, y0 - py);
    appendVertex(vertices, anchor, abgr, x1 - px, y1 - py);
    appendVertex(vertices, anchor, abgr, x1 + px, y1 + py);
    constexpr uint16_t quadIndices[] = {0, 1, 2, 0, 2, 3};
    for (const uint16_t index : quadIndices) {
        indices.push_back(static_cast<uint16_t>(base + index));
    }
}

void appendAxisLabel(std::vector<AxisVertex>& vertices,
                     std::vector<uint16_t>& indices,
                     char label,
                     const ayt::math::FVector3& direction,
                     uint32_t abgr)
{
    // The anchor sits just beyond the arrow tip in 3D. Individual strokes
    // are clip-space offsets, so letters remain upright as the camera turns.
    const ayt::math::FVector3 anchor = direction * 1.045f;
    constexpr float left = -0.065f;
    constexpr float right = 0.065f;
    constexpr float bottom = -0.085f;
    constexpr float middle = 0.0f;
    constexpr float top = 0.085f;
    constexpr float stroke = 0.025f;

    switch (label) {
    case 'X':
        appendLabelStroke(vertices, indices, anchor,
                          left, bottom, right, top, stroke, abgr);
        appendLabelStroke(vertices, indices, anchor,
                          left, top, right, bottom, stroke, abgr);
        break;
    case 'Y':
        appendLabelStroke(vertices, indices, anchor,
                          left, top, 0.0f, middle, stroke, abgr);
        appendLabelStroke(vertices, indices, anchor,
                          right, top, 0.0f, middle, stroke, abgr);
        appendLabelStroke(vertices, indices, anchor,
                          0.0f, middle, 0.0f, bottom, stroke, abgr);
        break;
    case 'Z':
        appendLabelStroke(vertices, indices, anchor,
                          left, top, right, top, stroke, abgr);
        appendLabelStroke(vertices, indices, anchor,
                          right, top, left, bottom, stroke, abgr);
        appendLabelStroke(vertices, indices, anchor,
                          left, bottom, right, bottom, stroke, abgr);
        break;
    default:
        break;
    }
}

void appendArrow(std::vector<AxisVertex>& vertices,
                 std::vector<uint16_t>& indices,
                 const ayt::math::FVector3& direction,
                 const ayt::math::FVector3& sideU,
                 const ayt::math::FVector3& sideV,
                 uint32_t abgr,
                 bool transformGizmo = false)
{
    // The corner orientation widget keeps its long arrows. Transform Gizmo
    // translation occupies only the outer radial band: a much thinner, short
    // arrow separated from the inner scale cube by visible negative space.
    const float shaftStart = transformGizmo ? 0.50f : 0.045f;
    const float shaftEnd = transformGizmo ? 0.81f : 0.755f;
    const float shaftRadius = transformGizmo ? 0.008f : 0.016f;
    const float headBase = transformGizmo ? 0.78f : 0.705f;
    const float headRadius = transformGizmo ? 0.040f : 0.058f;
    const float headTip = transformGizmo ? 0.95f : 0.985f;

    const ayt::math::FVector3 p0 = direction * shaftStart;
    const ayt::math::FVector3 p1 = direction * shaftEnd;
    const ayt::math::FVector3 u = sideU * shaftRadius;
    const ayt::math::FVector3 v = sideV * shaftRadius;

    const uint16_t shaftBase = static_cast<uint16_t>(vertices.size());
    const ayt::math::FVector3 shaftCorners[8] = {
        p0 - u - v, p0 + u - v, p0 + u + v, p0 - u + v,
        p1 - u - v, p1 + u - v, p1 + u + v, p1 - u + v,
    };
    for (const ayt::math::FVector3& corner : shaftCorners) {
        appendVertex(vertices, corner, abgr);
    }
    constexpr uint16_t shaftIndices[] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    };
    for (const uint16_t index : shaftIndices) {
        indices.push_back(static_cast<uint16_t>(shaftBase + index));
    }

    const ayt::math::FVector3 headCenter = direction * headBase;
    const ayt::math::FVector3 hu = sideU * headRadius;
    const ayt::math::FVector3 hv = sideV * headRadius;
    const uint16_t headBaseIndex = static_cast<uint16_t>(vertices.size());
    appendVertex(vertices, headCenter - hu - hv, abgr);
    appendVertex(vertices, headCenter + hu - hv, abgr);
    appendVertex(vertices, headCenter + hu + hv, abgr);
    appendVertex(vertices, headCenter - hu + hv, abgr);
    appendVertex(vertices, direction * headTip, abgr);
    constexpr uint16_t headIndices[] = {
        0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4,
        0, 2, 1, 0, 3, 2,
    };
    for (const uint16_t index : headIndices) {
        indices.push_back(static_cast<uint16_t>(headBaseIndex + index));
    }
}

void appendNegativeAxis(std::vector<AxisVertex>& vertices,
                        std::vector<uint16_t>& indices,
                        const ayt::math::FVector3& direction,
                        const ayt::math::FVector3& sideU,
                        const ayt::math::FVector3& sideV,
                        uint32_t abgr)
{
    // Negative directions are deliberately shorter, thinner and muted.
    // They disambiguate orientation without competing with the labeled
    // positive arrows or implying an additional selectable direction.
    constexpr float shaftStart = 0.035f;
    constexpr float shaftEnd = 0.46f;
    constexpr float shaftRadius = 0.010f;

    const ayt::math::FVector3 p0 = direction * shaftStart;
    const ayt::math::FVector3 p1 = direction * shaftEnd;
    const ayt::math::FVector3 u = sideU * shaftRadius;
    const ayt::math::FVector3 v = sideV * shaftRadius;
    const uint16_t base = static_cast<uint16_t>(vertices.size());
    const ayt::math::FVector3 corners[8] = {
        p0 - u - v, p0 + u - v, p0 + u + v, p0 - u + v,
        p1 - u - v, p1 + u - v, p1 + u + v, p1 - u + v,
    };
    for (const ayt::math::FVector3& corner : corners) {
        appendVertex(vertices, corner, abgr);
    }
    constexpr uint16_t shaftIndices[] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    };
    for (const uint16_t index : shaftIndices) {
        indices.push_back(static_cast<uint16_t>(base + index));
    }
}

void appendBox(std::vector<AxisVertex>& vertices,
               std::vector<uint16_t>& indices,
               const ayt::math::FVector3& center,
               const ayt::math::FVector3& halfExtent,
               uint32_t abgr)
{
    const uint16_t base = static_cast<uint16_t>(vertices.size());
    const ayt::math::FVector3 corners[8] = {
        center + ayt::math::FVector3(-halfExtent.x, -halfExtent.y, -halfExtent.z),
        center + ayt::math::FVector3( halfExtent.x, -halfExtent.y, -halfExtent.z),
        center + ayt::math::FVector3( halfExtent.x,  halfExtent.y, -halfExtent.z),
        center + ayt::math::FVector3(-halfExtent.x,  halfExtent.y, -halfExtent.z),
        center + ayt::math::FVector3(-halfExtent.x, -halfExtent.y,  halfExtent.z),
        center + ayt::math::FVector3( halfExtent.x, -halfExtent.y,  halfExtent.z),
        center + ayt::math::FVector3( halfExtent.x,  halfExtent.y,  halfExtent.z),
        center + ayt::math::FVector3(-halfExtent.x,  halfExtent.y,  halfExtent.z),
    };
    for (const auto& corner : corners) appendVertex(vertices, corner, abgr);
    constexpr uint16_t boxIndices[] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
        2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    };
    for (uint16_t index : boxIndices) {
        indices.push_back(static_cast<uint16_t>(base + index));
    }
}

void appendScaleAxis(std::vector<AxisVertex>& vertices,
                     std::vector<uint16_t>& indices,
                     int axis,
                     uint32_t abgr)
{
    ayt::math::FVector3 shaftCenter{};
    ayt::math::FVector3 shaftHalf(0.010f, 0.010f, 0.010f);
    ayt::math::FVector3 headCenter{};
    ayt::math::FVector3 headHalf(0.045f, 0.045f, 0.045f);
    shaftCenter[axis] = 0.47f;
    shaftHalf[axis] = 0.37f;
    headCenter[axis] = 0.88f;
    appendBox(vertices, indices, shaftCenter, shaftHalf, abgr);
    appendBox(vertices, indices, headCenter, headHalf, abgr);
}

void appendUniversalScaleHandle(std::vector<AxisVertex>& vertices,
                                std::vector<uint16_t>& indices,
                                int axis,
                                uint32_t abgr)
{
    // Inner thin stem + compact cube reads as scale, while translation has a
    // detached outer arrow. The gap between them is intentionally empty.
    ayt::math::FVector3 stemCenter{};
    ayt::math::FVector3 stemHalf(0.0075f, 0.0075f, 0.0075f);
    stemCenter[axis] = 0.20f;
    stemHalf[axis] = 0.11f;
    appendBox(vertices, indices, stemCenter, stemHalf, abgr);

    ayt::math::FVector3 center{};
    center[axis] = 0.35f;
    appendBox(vertices, indices, center, {0.032f, 0.032f, 0.032f}, abgr);
}

void appendPlaneHandle(std::vector<AxisVertex>& vertices,
                       std::vector<uint16_t>& indices,
                       int firstAxis,
                       int secondAxis,
                       uint32_t abgr)
{
    constexpr float minValue = 0.18f;
    constexpr float maxValue = 0.32f;
    constexpr float halfWidth = 0.010f;
    auto appendEdge = [&](float first0, float second0,
                          float first1, float second1) {
        const float dx = first1 - first0;
        const float dy = second1 - second0;
        const float length = std::max(0.0001f, std::sqrt(dx * dx + dy * dy));
        const float px = -dy / length * halfWidth;
        const float py =  dx / length * halfWidth;
        ayt::math::FVector3 points[4]{};
        points[0][firstAxis] = first0 + px;
        points[0][secondAxis] = second0 + py;
        points[1][firstAxis] = first0 - px;
        points[1][secondAxis] = second0 - py;
        points[2][firstAxis] = first1 - px;
        points[2][secondAxis] = second1 - py;
        points[3][firstAxis] = first1 + px;
        points[3][secondAxis] = second1 + py;
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        for (const auto& point : points) appendVertex(vertices, point, abgr);
        constexpr uint16_t quad[] = {0, 1, 2, 0, 2, 3};
        for (uint16_t index : quad) {
            indices.push_back(static_cast<uint16_t>(base + index));
        }
    };
    appendEdge(minValue, minValue, maxValue, minValue);
    appendEdge(maxValue, minValue, maxValue, maxValue);
    appendEdge(maxValue, maxValue, minValue, maxValue);
    appendEdge(minValue, maxValue, minValue, minValue);
}

void appendRing(std::vector<AxisVertex>& vertices,
                 std::vector<uint16_t>& indices,
                 int normalAxis,
                 uint32_t abgr,
                 float radius)
{
    constexpr int segments = 64;
    constexpr float halfWidth = 0.010f;
    constexpr float twoPi = 6.28318530717958647692f;
    int firstAxis = 0;
    int secondAxis = 1;
    if (normalAxis == 0) { firstAxis = 1; secondAxis = 2; }
    else if (normalAxis == 1) { firstAxis = 2; secondAxis = 0; }

    for (int segment = 0; segment < segments; ++segment) {
        const float a0 = twoPi * static_cast<float>(segment)
                       / static_cast<float>(segments);
        const float a1 = twoPi * static_cast<float>(segment + 1)
                       / static_cast<float>(segments);
        ayt::math::FVector3 points[4]{};
        points[0][firstAxis] = std::cos(a0) * (radius - halfWidth);
        points[0][secondAxis] = std::sin(a0) * (radius - halfWidth);
        points[1][firstAxis] = std::cos(a0) * (radius + halfWidth);
        points[1][secondAxis] = std::sin(a0) * (radius + halfWidth);
        points[2][firstAxis] = std::cos(a1) * (radius + halfWidth);
        points[2][secondAxis] = std::sin(a1) * (radius + halfWidth);
        points[3][firstAxis] = std::cos(a1) * (radius - halfWidth);
        points[3][secondAxis] = std::sin(a1) * (radius - halfWidth);
        const uint16_t base = static_cast<uint16_t>(vertices.size());
        for (const auto& point : points) appendVertex(vertices, point, abgr);
        constexpr uint16_t quad[] = {0, 1, 2, 0, 2, 3};
        for (uint16_t index : quad) {
            indices.push_back(static_cast<uint16_t>(base + index));
        }
    }
}

void appendGridLine(std::vector<AxisVertex>& vertices,
                    std::vector<uint16_t>& indices,
                    const ayt::math::FVector2& from,
                    const ayt::math::FVector2& to,
                    float halfWidth,
                    uint32_t abgr)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 1.0e-6f) || vertices.size() > 65530u) return;
    const float px = -dy / length * halfWidth;
    const float py =  dx / length * halfWidth;
    const uint16_t base = static_cast<uint16_t>(vertices.size());
    appendVertex(vertices, {from.x + px, from.y + py, 0.0f}, abgr);
    appendVertex(vertices, {from.x - px, from.y - py, 0.0f}, abgr);
    appendVertex(vertices, {to.x - px, to.y - py, 0.0f}, abgr);
    appendVertex(vertices, {to.x + px, to.y + py, 0.0f}, abgr);
    constexpr uint16_t quad[] = {0, 1, 2, 0, 2, 3};
    for (uint16_t index : quad) {
        indices.push_back(static_cast<uint16_t>(base + index));
    }
}

void appendSelectionLine(std::vector<AxisVertex>& vertices,
                         std::vector<uint16_t>& indices,
                         const ayt::math::FVector3& from,
                         const ayt::math::FVector3& to,
                         float halfWidth,
                         uint32_t abgr)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 1.0e-6f) || vertices.size() > 65530u) return;
    const float px = -dy / length * halfWidth;
    const float py =  dx / length * halfWidth;
    const uint16_t base = static_cast<uint16_t>(vertices.size());
    appendVertex(vertices, {from.x + px, from.y + py, from.z}, abgr);
    appendVertex(vertices, {from.x - px, from.y - py, from.z}, abgr);
    appendVertex(vertices, {to.x - px, to.y - py, to.z}, abgr);
    appendVertex(vertices, {to.x + px, to.y + py, to.z}, abgr);
    constexpr uint16_t quad[] = {0, 1, 2, 0, 2, 3};
    for (uint16_t index : quad) {
        indices.push_back(static_cast<uint16_t>(base + index));
    }
}

bool sameSelectionOutline2DState(
    const EditorSelectionOutline2DState& lhs,
    const EditorSelectionOutline2DState& rhs) noexcept
{
    if (lhs.visible != rhs.visible
        || lhs.lineWidthWorld != rhs.lineWidthWorld) {
        return false;
    }
    for (int index = 0; index < 4; ++index) {
        if (lhs.corners[index].x != rhs.corners[index].x
            || lhs.corners[index].y != rhs.corners[index].y
            || lhs.corners[index].z != rhs.corners[index].z) {
            return false;
        }
    }
    return true;
}

bool sameGridState(const EditorGrid2DState& lhs,
                   const EditorGrid2DState& rhs) noexcept
{
    return lhs.visible == rhs.visible
        && lhs.center.x == rhs.center.x
        && lhs.center.y == rhs.center.y
        && lhs.verticalWorldSize == rhs.verticalWorldSize
        && lhs.minorSpacing == rhs.minorSpacing
        && lhs.majorEvery == rhs.majorEvery;
}

} // namespace

ayt::math::Float4x4 EditorOverlayPass::makeOrientationAxisView(
    const ayt::math::Float4x4& sceneView)
{
    ayt::math::Float4x4 axisView = sceneView;
    axisView.row[0].w = 0.0f;
    axisView.row[1].w = 0.0f;
    axisView.row[2].w = 3.0f;
    axisView.row[3] = ayt::math::FVector4(0.0f, 0.0f, 0.0f, 1.0f);
    return axisView;
}

const char* EditorOverlayPass::orientationAxisVaryingDef() noexcept
{
    return kAxisVaryingDef;
}

const char* EditorOverlayPass::orientationAxisVertexShader() noexcept
{
    return kAxisVertexShader;
}

const char* EditorOverlayPass::orientationAxisFragmentShader() noexcept
{
    return kAxisFragmentShader;
}

bool EditorOverlayPass::ensureOrientationAxisResources(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    if (!BGFXAdapter::isValid(_axisVertexBuffer)
        || !BGFXAdapter::isValid(_axisIndexBuffer)) {
        if (BGFXAdapter::isValid(_axisVertexBuffer)) {
            adapter.destroy(_axisVertexBuffer);
            _axisVertexBuffer = BGFX_INVALID_HANDLE;
        }
        if (BGFXAdapter::isValid(_axisIndexBuffer)) {
            adapter.destroy(_axisIndexBuffer);
            _axisIndexBuffer = BGFX_INVALID_HANDLE;
        }

        std::vector<AxisVertex> vertices;
        std::vector<uint16_t> indices;
        vertices.reserve(95);
        indices.reserve(318);
        appendArrow(vertices, indices,
                    ayt::math::FVector3(1.0f, 0.0f, 0.0f),
                    ayt::math::FVector3(0.0f, 1.0f, 0.0f),
                    ayt::math::FVector3(0.0f, 0.0f, 1.0f),
                    0xff4f4fe8u); // X: red
        appendArrow(vertices, indices,
                    ayt::math::FVector3(0.0f, 1.0f, 0.0f),
                    ayt::math::FVector3(1.0f, 0.0f, 0.0f),
                    ayt::math::FVector3(0.0f, 0.0f, 1.0f),
                    0xff67c956u); // Y: green
        appendArrow(vertices, indices,
                    ayt::math::FVector3(0.0f, 0.0f, 1.0f),
                    ayt::math::FVector3(1.0f, 0.0f, 0.0f),
                    ayt::math::FVector3(0.0f, 1.0f, 0.0f),
                    0xffff8d4du); // Z: blue

        appendNegativeAxis(vertices, indices,
                           ayt::math::FVector3(-1.0f, 0.0f, 0.0f),
                           ayt::math::FVector3(0.0f, 1.0f, 0.0f),
                           ayt::math::FVector3(0.0f, 0.0f, 1.0f),
                           0xff62628au); // -X: muted red
        appendNegativeAxis(vertices, indices,
                           ayt::math::FVector3(0.0f, -1.0f, 0.0f),
                           ayt::math::FVector3(1.0f, 0.0f, 0.0f),
                           ayt::math::FVector3(0.0f, 0.0f, 1.0f),
                           0xff647b5fu); // -Y: muted green
        appendNegativeAxis(vertices, indices,
                           ayt::math::FVector3(0.0f, 0.0f, -1.0f),
                           ayt::math::FVector3(1.0f, 0.0f, 0.0f),
                           ayt::math::FVector3(0.0f, 1.0f, 0.0f),
                           0xff88715fu); // -Z: muted blue

        _axisArrowIndexCount = static_cast<uint32_t>(indices.size());
        _axisLabelIndexStart = _axisArrowIndexCount;
        appendAxisLabel(vertices, indices, 'X',
                        ayt::math::FVector3(1.0f, 0.0f, 0.0f),
                        0xff4f4fe8u);
        appendAxisLabel(vertices, indices, 'Y',
                        ayt::math::FVector3(0.0f, 1.0f, 0.0f),
                        0xff67c956u);
        appendAxisLabel(vertices, indices, 'Z',
                        ayt::math::FVector3(0.0f, 0.0f, 1.0f),
                        0xffff8d4du);
        _axisLabelIndexCount = static_cast<uint32_t>(indices.size())
                             - _axisLabelIndexStart;

        const bgfx::VertexLayout layout =
            adapter.vertexLayoutPosScreenOffsetColor();
        _axisVertexBuffer = adapter.createVertexBuffer(
            vertices.data(),
            static_cast<uint32_t>(vertices.size() * sizeof(AxisVertex)),
            layout);
        _axisIndexBuffer = adapter.createIndexBuffer(
            indices.data(),
            static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
        if (!BGFXAdapter::isValid(_axisVertexBuffer)
            || !BGFXAdapter::isValid(_axisIndexBuffer)) {
            destroyResources(adapter);
            return false;
        }
    }

    if (!_axisProgram.isValid() && !_axisProgramAcquireFailed) {
        _axisProgram = ctx.pool.acquireFromBgfxSc(
            kAxisVertexShader,
            kAxisFragmentShader,
            kAxisVaryingDef,
            kAxisShaderCacheKey);
        if (!_axisProgram.isValid()) {
            _axisProgramAcquireFailed = true;
            std::fprintf(stderr,
                         "[EditorOverlayPass] orientation-axis shader "
                         "acquire failed; widget disabled for this context.\n");
            for (const std::string& error : ctx.pool.lastCompileErrors()) {
                std::fprintf(stderr, "[EditorOverlayPass]   %s\n", error.c_str());
            }
        }
    }
    return BGFXAdapter::isValid(_axisVertexBuffer)
        && BGFXAdapter::isValid(_axisIndexBuffer)
        && _axisArrowIndexCount != 0
        && _axisLabelIndexCount != 0
        && _axisProgram.isValid();
}

uint32_t EditorOverlayPass::submitOrientationAxis(PassExecContext& ctx,
                                                   const FrameContext& frame)
{
    if (!_orientationAxisEnabled || !ensureOrientationAxisResources(ctx)) {
        return 0;
    }

    constexpr uint16_t preferredSize = 96;
    constexpr uint16_t preferredPadding = 12;
    constexpr uint16_t minimumSize = 40;
    const uint16_t available = std::min(ctx.viewportWidth, ctx.viewportHeight);
    if (available < minimumSize + 4u) {
        return 0;
    }
    const uint16_t padding = std::min<uint16_t>(
        preferredPadding, static_cast<uint16_t>((available - minimumSize) / 2u));
    const uint16_t axisSize = std::min<uint16_t>(
        preferredSize, static_cast<uint16_t>(available - padding * 2u));
    if (axisSize < minimumSize) {
        return 0;
    }

    const uint16_t axisX = static_cast<uint16_t>(ctx.viewportX + padding);
    const uint16_t axisY = static_cast<uint16_t>(
        ctx.viewportY + ctx.viewportHeight - padding - axisSize);
    const ayt::math::Float4x4 axisView = makeOrientationAxisView(frame.view);
    const ayt::math::Float4x4 axisProjection = ayt::math::lh::ortho(
        -1.18f, 1.18f, -1.18f, 1.18f, 0.1f, 10.0f);

    ctx.adapter.setViewFrameBuffer(kAxisViewId, BGFX_INVALID_HANDLE);
    ctx.adapter.setViewRect(kAxisViewId, axisX, axisY, axisSize, axisSize);
    ctx.adapter.setViewTransform(kAxisViewId, axisView, axisProjection);
    // The clear is limited to the 96px widget rect. It establishes a private
    // depth surface for arrow self-occlusion and never touches scene color.
    ctx.adapter.setViewClearRaw(kAxisViewId, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    ctx.adapter.setTransformIdentity();
    ctx.adapter.setVertexBuffer(_axisVertexBuffer);
    ctx.adapter.setIndexBuffer(_axisIndexBuffer, 0, _axisArrowIndexCount);
    ctx.adapter.setStateOverlayDepthWrite();

    ayt::shader::DrawCallContext draw;
    draw.viewId = kAxisViewId;
    draw.state = 0;
    _axisProgram.submit(draw);

    // Labels are submitted after the arrows but share the widget's private
    // depth surface. This resolves label-vs-label and rear-label-vs-front-axis
    // occlusion from the current camera instead of relying on XYZ draw order.
    ctx.adapter.setTransformIdentity();
    ctx.adapter.setVertexBuffer(_axisVertexBuffer);
    ctx.adapter.setIndexBuffer(_axisIndexBuffer,
                               _axisLabelIndexStart,
                               _axisLabelIndexCount);
    ctx.adapter.setStateOverlayDepthWrite();
    _axisProgram.submit(draw);
    return 2;
}

void EditorOverlayPass::destroyTransformGizmoGeometry(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_gizmoVertexBuffer)) {
        adapter.destroy(_gizmoVertexBuffer);
        _gizmoVertexBuffer = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_gizmoIndexBuffer)) {
        adapter.destroy(_gizmoIndexBuffer);
        _gizmoIndexBuffer = BGFX_INVALID_HANDLE;
    }
    _gizmoPrimaryIndexCount = 0;
    _gizmoIndexCount = 0;
    _builtGizmoMode = EditorTransformGizmoMode::Hidden;
    _builtGizmoHighlight = 0xffu;
    _builtGizmoDisabledHandleMask = 0xffffu;
}

bool EditorOverlayPass::ensureTransformGizmoResources(PassExecContext& ctx)
{
    if (!_transformGizmo.visible
        || _transformGizmo.mode == EditorTransformGizmoMode::Hidden) {
        return false;
    }
    if (!ensureOrientationAxisResources(ctx)) return false;
    if (BGFXAdapter::isValid(_gizmoVertexBuffer)
        && BGFXAdapter::isValid(_gizmoIndexBuffer)
        && _gizmoIndexCount != 0
        && _builtGizmoMode == _transformGizmo.mode
        && _builtGizmoHighlight == _transformGizmo.activeHandle
        && _builtGizmoDisabledHandleMask
            == _transformGizmo.disabledHandleMask) {
        return true;
    }

    destroyTransformGizmoGeometry(ctx.adapter);
    std::vector<AxisVertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(1536);
    indices.reserve(3072);
    const uint8_t highlight = _transformGizmo.activeHandle;
    const uint16_t disabledHandles = _transformGizmo.disabledHandleMask;
    const auto color = [highlight, disabledHandles](
                           uint32_t normal, uint8_t handle) noexcept {
        return gizmoColor(normal, handle, highlight, disabledHandles);
    };

    if (_transformGizmo.mode == EditorTransformGizmoMode::Translate
        || _transformGizmo.mode == EditorTransformGizmoMode::Universal) {
        appendArrow(vertices, indices,
                    {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f},
                    color(kGizmoX, 1u), true);
        appendArrow(vertices, indices,
                    {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f},
                    color(kGizmoY, 2u), true);
        appendArrow(vertices, indices,
                    {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f},
                    {0.0f, 1.0f, 0.0f},
                    color(kGizmoZ, 3u), true);
        appendPlaneHandle(vertices, indices, 0, 1,
                          color(kGizmoXY, 4u));
        appendPlaneHandle(vertices, indices, 1, 2,
                          color(kGizmoYZ, 5u));
        appendPlaneHandle(vertices, indices, 2, 0,
                          color(kGizmoZX, 6u));
    }
    if (_transformGizmo.mode == EditorTransformGizmoMode::Rotate
        || _transformGizmo.mode == EditorTransformGizmoMode::Universal) {
        const float ringRadius =
            _transformGizmo.mode == EditorTransformGizmoMode::Universal
                ? 1.03f : 0.82f;
        appendRing(vertices, indices, 0,
                   color(kGizmoX, 7u), ringRadius);
        appendRing(vertices, indices, 1,
                   color(kGizmoY, 8u), ringRadius);
        appendRing(vertices, indices, 2,
                   color(kGizmoZ, 9u), ringRadius);
    }
    if (_transformGizmo.mode == EditorTransformGizmoMode::Universal) {
        // Translation/rotation can use World basis while component scale must
        // remain in Local basis. Keep one buffer but split it into two draw
        // ranges so submitTransformGizmo can apply the correct transforms.
        _gizmoPrimaryIndexCount = static_cast<uint32_t>(indices.size());
        appendUniversalScaleHandle(vertices, indices, 0,
                        color(kGizmoX, 10u));
        appendUniversalScaleHandle(vertices, indices, 1,
                        color(kGizmoY, 11u));
        appendUniversalScaleHandle(vertices, indices, 2,
                        color(kGizmoZ, 12u));
        appendBox(vertices, indices, {}, {0.055f, 0.055f, 0.055f},
                  color(kGizmoUniform, 13u));
    } else if (_transformGizmo.mode == EditorTransformGizmoMode::Scale) {
        appendScaleAxis(vertices, indices, 0,
                        color(kGizmoX, 10u));
        appendScaleAxis(vertices, indices, 1,
                        color(kGizmoY, 11u));
        appendScaleAxis(vertices, indices, 2,
                        color(kGizmoZ, 12u));
        appendBox(vertices, indices, {}, {0.055f, 0.055f, 0.055f},
                  color(kGizmoUniform, 13u));
    }

    if (vertices.empty() || indices.empty()) return false;
    if (_gizmoPrimaryIndexCount == 0) {
        _gizmoPrimaryIndexCount = static_cast<uint32_t>(indices.size());
    }
    const bgfx::VertexLayout layout =
        ctx.adapter.vertexLayoutPosScreenOffsetColor();
    _gizmoVertexBuffer = ctx.adapter.createVertexBuffer(
        vertices.data(),
        static_cast<uint32_t>(vertices.size() * sizeof(AxisVertex)),
        layout);
    _gizmoIndexBuffer = ctx.adapter.createIndexBuffer(
        indices.data(),
        static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
    if (!BGFXAdapter::isValid(_gizmoVertexBuffer)
        || !BGFXAdapter::isValid(_gizmoIndexBuffer)) {
        destroyTransformGizmoGeometry(ctx.adapter);
        return false;
    }
    _gizmoIndexCount = static_cast<uint32_t>(indices.size());
    _builtGizmoMode = _transformGizmo.mode;
    _builtGizmoHighlight = highlight;
    _builtGizmoDisabledHandleMask = disabledHandles;
    return true;
}

uint32_t EditorOverlayPass::submitTransformGizmo(
    PassExecContext& ctx,
    const FrameContext& frame)
{
    if (!ensureTransformGizmoResources(ctx)) return 0;
    const auto& state = _transformGizmo;
    const float distance = (state.position - frame.cameraPosition).length();
    if (!std::isfinite(distance)) return 0;
    const float scale = std::isfinite(state.worldScaleOverride)
            && state.worldScaleOverride > 0.0f
        ? std::clamp(state.worldScaleOverride, 0.12f, 1000.0f)
        : std::clamp(distance * kEditorTransformGizmoScalePerDistance,
                     0.12f, 1000.0f);
    ctx.adapter.setViewFrameBuffer(kGizmoViewId, BGFX_INVALID_HANDLE);
    ctx.adapter.setViewRect(kGizmoViewId, ctx.viewportX, ctx.viewportY,
                            ctx.viewportWidth, ctx.viewportHeight);
    const ayt::math::Float4x4& overlayProjection =
        _hasUnjitteredProjection
            ? _unjitteredProjection
            : frame.projection;
    ctx.adapter.setViewTransform(kGizmoViewId, frame.view, overlayProjection);
    ctx.adapter.setViewMode(kGizmoViewId, bgfx::ViewMode::Sequential);
    // Present has already written color. Clear only depth so the gizmo is
    // always reachable while retaining correct self-occlusion.
    ctx.adapter.setViewClearRaw(kGizmoViewId, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
    ayt::shader::DrawCallContext draw;
    draw.viewId = kGizmoViewId;
    draw.state = 0;

    auto submitRange = [&](const ayt::math::FQuaternion& rotation,
                           uint32_t firstIndex,
                           uint32_t indexCount) {
        if (indexCount == 0) return 0u;
        const ayt::math::Float4x4 world = ayt::math::Float4x4::fromTRS(
            state.position, rotation, {scale, scale, scale});
        ctx.adapter.setTransform(world);
        ctx.adapter.setVertexBuffer(_gizmoVertexBuffer);
        ctx.adapter.setIndexBuffer(
            _gizmoIndexBuffer, firstIndex, indexCount);
        ctx.adapter.setStateOverlayDepthWrite();
        _axisProgram.submit(draw);
        return 1u;
    };

    if (state.mode == EditorTransformGizmoMode::Universal
        && !state.localSpace) {
        const uint32_t primaryCount = std::min(
            _gizmoPrimaryIndexCount, _gizmoIndexCount);
        return submitRange(ayt::math::FQuaternion::identity(),
                           0, primaryCount)
             + submitRange(state.rotation, primaryCount,
                           _gizmoIndexCount - primaryCount);
    }

    const bool useLocalBasis = state.localSpace
        || state.mode == EditorTransformGizmoMode::Scale
        || state.mode == EditorTransformGizmoMode::Universal;
    return submitRange(useLocalBasis
                           ? state.rotation
                           : ayt::math::FQuaternion::identity(),
                       0, _gizmoIndexCount);
}

void EditorOverlayPass::destroyGrid2DGeometry(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_gridVertexBuffer)) {
        adapter.destroy(_gridVertexBuffer);
        _gridVertexBuffer = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_gridIndexBuffer)) {
        adapter.destroy(_gridIndexBuffer);
        _gridIndexBuffer = BGFX_INVALID_HANDLE;
    }
    _gridIndexCount = 0;
    _builtGridViewportWidth = 0;
    _builtGridViewportHeight = 0;
    _builtGrid2D = {};
}

bool EditorOverlayPass::ensureGrid2DResources(PassExecContext& ctx)
{
    if (!_grid2D.visible || !std::isfinite(_grid2D.center.x)
        || !std::isfinite(_grid2D.center.y)
        || !std::isfinite(_grid2D.verticalWorldSize)
        || !std::isfinite(_grid2D.minorSpacing)
        || _grid2D.verticalWorldSize <= 0.0f
        || _grid2D.minorSpacing <= 0.0f
        || ctx.viewportWidth == 0 || ctx.viewportHeight == 0) {
        return false;
    }
    if (BGFXAdapter::isValid(_gridVertexBuffer)
        && BGFXAdapter::isValid(_gridIndexBuffer)
        && _gridIndexCount != 0
        && _builtGridViewportWidth == ctx.viewportWidth
        && _builtGridViewportHeight == ctx.viewportHeight
        && sameGridState(_builtGrid2D, _grid2D)) {
        return true;
    }

    destroyGrid2DGeometry(ctx.adapter);
    if (!ensureOrientationAxisResources(ctx)) return false;

    const float aspect = static_cast<float>(ctx.viewportWidth)
                       / static_cast<float>(ctx.viewportHeight);
    const float halfH = _grid2D.verticalWorldSize * 0.5f;
    const float halfW = halfH * aspect;
    const float spacing = _grid2D.minorSpacing;
    const float minX = _grid2D.center.x - halfW - spacing;
    const float maxX = _grid2D.center.x + halfW + spacing;
    const float minY = _grid2D.center.y - halfH - spacing;
    const float maxY = _grid2D.center.y + halfH + spacing;
    const float halfLineWidth = std::max(
        _grid2D.verticalWorldSize
            / static_cast<float>(ctx.viewportHeight) * 0.45f,
        spacing * 0.0005f);
    const int firstX = static_cast<int>(std::floor(minX / spacing));
    const int lastX = static_cast<int>(std::ceil(maxX / spacing));
    const int firstY = static_cast<int>(std::floor(minY / spacing));
    const int lastY = static_cast<int>(std::ceil(maxY / spacing));
    if ((lastX - firstX) > 1024 || (lastY - firstY) > 1024) return false;

    std::vector<AxisVertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(static_cast<std::size_t>(
        std::max(0, lastX - firstX + lastY - firstY + 2)) * 4u);
    indices.reserve(vertices.capacity() / 4u * 6u);
    const uint32_t majorEvery = std::max(1u, _grid2D.majorEvery);
    constexpr uint32_t minorColor = 0x243e4650u;
    constexpr uint32_t majorColor = 0x4057626eu;
    constexpr uint32_t xAxisColor = 0x806060d8u;
    constexpr uint32_t yAxisColor = 0x8067b858u;

    for (int index = firstX; index <= lastX; ++index) {
        const float x = static_cast<float>(index) * spacing;
        const bool axis = index == 0;
        const bool major = (std::abs(index) % static_cast<int>(majorEvery)) == 0;
        appendGridLine(vertices, indices, {x, minY}, {x, maxY}, halfLineWidth,
                       axis ? yAxisColor : (major ? majorColor : minorColor));
    }
    for (int index = firstY; index <= lastY; ++index) {
        const float y = static_cast<float>(index) * spacing;
        const bool axis = index == 0;
        const bool major = (std::abs(index) % static_cast<int>(majorEvery)) == 0;
        appendGridLine(vertices, indices, {minX, y}, {maxX, y}, halfLineWidth,
                       axis ? xAxisColor : (major ? majorColor : minorColor));
    }
    if (vertices.empty() || indices.empty()) return false;

    const bgfx::VertexLayout layout =
        ctx.adapter.vertexLayoutPosScreenOffsetColor();
    _gridVertexBuffer = ctx.adapter.createVertexBuffer(
        vertices.data(),
        static_cast<uint32_t>(vertices.size() * sizeof(AxisVertex)), layout);
    _gridIndexBuffer = ctx.adapter.createIndexBuffer(
        indices.data(),
        static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
    if (!BGFXAdapter::isValid(_gridVertexBuffer)
        || !BGFXAdapter::isValid(_gridIndexBuffer)) {
        destroyGrid2DGeometry(ctx.adapter);
        return false;
    }
    _gridIndexCount = static_cast<uint32_t>(indices.size());
    _builtGrid2D = _grid2D;
    _builtGridViewportWidth = ctx.viewportWidth;
    _builtGridViewportHeight = ctx.viewportHeight;
    return true;
}

uint32_t EditorOverlayPass::submitGrid2D(PassExecContext& ctx,
                                         const FrameContext& frame)
{
    if (!ensureGrid2DResources(ctx)) return 0;
    ctx.adapter.setViewFrameBuffer(kGridViewId, BGFX_INVALID_HANDLE);
    ctx.adapter.setViewRect(kGridViewId, ctx.viewportX, ctx.viewportY,
                            ctx.viewportWidth, ctx.viewportHeight);
    const ayt::math::Float4x4& projection = _hasUnjitteredProjection
        ? _unjitteredProjection : frame.projection;
    ctx.adapter.setViewTransform(kGridViewId, frame.view, projection);
    ctx.adapter.setViewMode(kGridViewId, bgfx::ViewMode::Sequential);
    ctx.adapter.setTransformIdentity();
    ctx.adapter.setVertexBuffer(_gridVertexBuffer);
    ctx.adapter.setIndexBuffer(_gridIndexBuffer, 0, _gridIndexCount);
    ctx.adapter.setStateAlphaBlend();
    ayt::shader::DrawCallContext draw;
    draw.viewId = kGridViewId;
    draw.state = 0;
    _axisProgram.submit(draw);
    return 1;
}

void EditorOverlayPass::destroySelectionOutline2DGeometry(
    BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_selectionOutline2DVertexBuffer)) {
        adapter.destroy(_selectionOutline2DVertexBuffer);
        _selectionOutline2DVertexBuffer = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_selectionOutline2DIndexBuffer)) {
        adapter.destroy(_selectionOutline2DIndexBuffer);
        _selectionOutline2DIndexBuffer = BGFX_INVALID_HANDLE;
    }
    _selectionOutline2DIndexCount = 0;
    _builtSelectionOutline2D = {};
}

bool EditorOverlayPass::ensureSelectionOutline2DResources(
    PassExecContext& ctx)
{
    const auto& state = _selectionOutline2D;
    if (!state.visible || !std::isfinite(state.lineWidthWorld)
        || state.lineWidthWorld <= 0.0f) {
        return false;
    }
    for (const auto& corner : state.corners) {
        if (!std::isfinite(corner.x) || !std::isfinite(corner.y)
            || !std::isfinite(corner.z)) {
            return false;
        }
    }
    if (BGFXAdapter::isValid(_selectionOutline2DVertexBuffer)
        && BGFXAdapter::isValid(_selectionOutline2DIndexBuffer)
        && _selectionOutline2DIndexCount != 0
        && sameSelectionOutline2DState(_builtSelectionOutline2D, state)) {
        return true;
    }

    destroySelectionOutline2DGeometry(ctx.adapter);
    if (!ensureOrientationAxisResources(ctx)) return false;
    std::vector<AxisVertex> vertices;
    std::vector<uint16_t> indices;
    vertices.reserve(16u);
    indices.reserve(24u);
    constexpr uint32_t outlineColor = 0xff39a8ffu;
    const float halfWidth = state.lineWidthWorld * 0.5f;
    for (int index = 0; index < 4; ++index) {
        appendSelectionLine(vertices, indices, state.corners[index],
                            state.corners[(index + 1) % 4], halfWidth,
                            outlineColor);
    }
    if (vertices.empty() || indices.empty()) return false;
    const bgfx::VertexLayout layout =
        ctx.adapter.vertexLayoutPosScreenOffsetColor();
    _selectionOutline2DVertexBuffer = ctx.adapter.createVertexBuffer(
        vertices.data(),
        static_cast<uint32_t>(vertices.size() * sizeof(AxisVertex)), layout);
    _selectionOutline2DIndexBuffer = ctx.adapter.createIndexBuffer(
        indices.data(),
        static_cast<uint32_t>(indices.size() * sizeof(uint16_t)));
    if (!BGFXAdapter::isValid(_selectionOutline2DVertexBuffer)
        || !BGFXAdapter::isValid(_selectionOutline2DIndexBuffer)) {
        destroySelectionOutline2DGeometry(ctx.adapter);
        return false;
    }
    _selectionOutline2DIndexCount = static_cast<uint32_t>(indices.size());
    _builtSelectionOutline2D = state;
    return true;
}

uint32_t EditorOverlayPass::submitSelectionOutline2D(
    PassExecContext& ctx, const FrameContext& frame)
{
    if (!ensureSelectionOutline2DResources(ctx)) return 0;
    ctx.adapter.setViewFrameBuffer(kGridViewId, BGFX_INVALID_HANDLE);
    ctx.adapter.setViewRect(kGridViewId, ctx.viewportX, ctx.viewportY,
                            ctx.viewportWidth, ctx.viewportHeight);
    const ayt::math::Float4x4& projection = _hasUnjitteredProjection
        ? _unjitteredProjection : frame.projection;
    ctx.adapter.setViewTransform(kGridViewId, frame.view, projection);
    ctx.adapter.setViewMode(kGridViewId, bgfx::ViewMode::Sequential);
    ctx.adapter.setTransformIdentity();
    ctx.adapter.setVertexBuffer(_selectionOutline2DVertexBuffer);
    ctx.adapter.setIndexBuffer(_selectionOutline2DIndexBuffer, 0,
                               _selectionOutline2DIndexCount);
    ctx.adapter.setStateAlphaBlend();
    ayt::shader::DrawCallContext draw;
    draw.viewId = kGridViewId;
    draw.state = 0;
    _axisProgram.submit(draw);
    return 1;
}

void EditorOverlayPass::destroyResources(BGFXAdapter& adapter)
{
    destroySelectionOutline2DGeometry(adapter);
    destroyGrid2DGeometry(adapter);
    destroyTransformGizmoGeometry(adapter);
    if (BGFXAdapter::isValid(_axisVertexBuffer)) {
        adapter.destroy(_axisVertexBuffer);
        _axisVertexBuffer = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_axisIndexBuffer)) {
        adapter.destroy(_axisIndexBuffer);
        _axisIndexBuffer = BGFX_INVALID_HANDLE;
    }
    _axisArrowIndexCount = 0;
    _axisLabelIndexStart = 0;
    _axisLabelIndexCount = 0;
    _axisProgram.reset();
    _axisProgramAcquireFailed = false;
}

uint32_t EditorOverlayPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    const FrameContext& frame = ctx.frame;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;

    if (!adapter.isInitialized() || adapter.isNoopBackend()) {
        return 0;
    }
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }

    return submitOrientationAxis(ctx, frame)
         + submitGrid2D(ctx, frame)
         + submitSelectionOutline2D(ctx, frame)
         + submitTransformGizmo(ctx, frame);
}

} // namespace ayt::render::detail
