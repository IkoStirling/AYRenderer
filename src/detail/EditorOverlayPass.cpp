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
                 uint32_t abgr)
{
    // Keep the widget light and readable: long narrow shafts with compact
    // heads leave more negative space between the three projected axes.
    constexpr float shaftStart = 0.045f;
    constexpr float shaftEnd = 0.755f;
    constexpr float shaftRadius = 0.016f;
    constexpr float headBase = 0.705f;
    constexpr float headRadius = 0.058f;
    constexpr float headTip = 0.985f;

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

void EditorOverlayPass::destroyResources(BGFXAdapter& adapter)
{
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

    return submitOrientationAxis(ctx, frame);
}

} // namespace ayt::render::detail
