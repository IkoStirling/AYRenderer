#include "detail/SMAAPass.h"

#include "detail/FgResource.h"
#include "detail/GpuResources.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ayt::render::detail
{

namespace {

// High-preset shader structure and Area/Search lookup contracts are adapted
// from the SMAA reference implementation by Jorge Jimenez et al. (MIT). See
// THIRD_PARTY_NOTICES.md. The lookup bytes remain programmatically generated.
// This three-stage reference port is a controlled raw-bgfx compatibility
// exception: it relies on many user helper functions, C-style bounded loops,
// and nested branches that Phoskia cannot yet represent without a semantic
// rewrite. Keep it on the tested .sc path until those language features and
// cross-backend shaderc coverage are available.
constexpr const char* kSmaaVaryingSc = R"(
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 a_position  : POSITION;
vec2 a_texcoord0 : TEXCOORD0;
)";

constexpr const char* kSmaaVertexSc = R"(
$input a_position, a_texcoord0
$output v_texcoord0
#include <bgfx_shader.sh>
void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
}
)";

constexpr const char* kSmaaEdgeFragmentSc = R"(
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(inputColor, 0);
uniform vec4 smaaMetrics;
uniform vec4 smaaParams;

vec2 smaaClampUv(vec2 uv)
{
    return clamp(uv, vec2(0.0, 0.0), vec2(1.0, 1.0));
}

float smaaColorDelta(vec3 a, vec3 b)
{
    vec3 delta = abs(a - b);
    return max(max(delta.r, delta.g), delta.b);
}

void main()
{
    vec2 uv = smaaClampUv(v_texcoord0);
    vec2 texel = smaaMetrics.xy;
    vec3 center = texture2D(inputColor, uv).rgb;
    vec3 left = texture2D(inputColor,
        smaaClampUv(uv - vec2(texel.x, 0.0))).rgb;
    vec3 top = texture2D(inputColor,
        smaaClampUv(uv - vec2(0.0, texel.y))).rgb;

    // Official SMAA color-edge detection. A pure luma gate misses saturated
    // material boundaries with similar luminance (orange against gray-green is
    // common in the editor validation scene), making a running pass look inert.
    vec2 delta = vec2(smaaColorDelta(center, left),
                      smaaColorDelta(center, top));
    vec2 edges = step(vec2(smaaParams.x, smaaParams.x), delta);
    if (dot(edges, vec2(1.0, 1.0)) == 0.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 right = texture2D(inputColor,
        smaaClampUv(uv + vec2(texel.x, 0.0))).rgb;
    vec3 bottom = texture2D(inputColor,
        smaaClampUv(uv + vec2(0.0, texel.y))).rgb;
    vec2 oppositeDelta = vec2(smaaColorDelta(center, right),
                              smaaColorDelta(center, bottom));
    vec2 maxDelta = max(delta, oppositeDelta);

    vec3 leftLeft = texture2D(inputColor,
        smaaClampUv(uv - vec2(2.0 * texel.x, 0.0))).rgb;
    vec3 topTop = texture2D(inputColor,
        smaaClampUv(uv - vec2(0.0, 2.0 * texel.y))).rgb;
    vec2 secondDelta = vec2(smaaColorDelta(left, leftLeft),
                            smaaColorDelta(top, topTop));
    maxDelta = max(maxDelta, secondDelta);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    // Local contrast adaptation rejects weak texture detail beside a stronger
    // edge, one of the main reasons SMAA stays sharper than a broad FXAA blur.
    edges *= step(vec2(finalDelta, finalDelta), smaaParams.y * delta);
    gl_FragColor = vec4(edges, 0.0, 1.0);
}
)";

constexpr const char* kSmaaWeightFragmentSc = R"(
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(edgesTex, 0);
SAMPLER2D(areaTex, 1);
SAMPLER2D(searchTex, 2);
uniform vec4 smaaMetrics;

vec2 smaaClampUv(vec2 uv)
{
    return clamp(uv, vec2(0.0, 0.0), vec2(1.0, 1.0));
}

vec2 smaaEdges(vec2 uv)
{
    return texture2D(edgesTex, smaaClampUv(uv)).rg;
}

vec2 smaaEdgesOffset(vec2 uv, vec2 offsetPixels)
{
    return smaaEdges(uv + offsetPixels * smaaMetrics.xy);
}

float smaaSearchLength(vec2 edge, float horizontalOffset)
{
    vec2 searchSize = vec2(66.0, 33.0);
    vec2 packedSize = vec2(64.0, 16.0);
    vec2 scale = searchSize * vec2(0.5, -1.0) + vec2(-1.0, 1.0);
    vec2 bias = searchSize * vec2(horizontalOffset, 1.0)
        + vec2(0.5, -0.5);
    vec2 uv = (scale * edge + bias) / packedSize;
    return texture2D(searchTex, smaaClampUv(uv)).r;
}

float smaaSearchXLeft(vec2 coord, float end)
{
    vec2 edge = vec2(0.0, 1.0);
    for (int i = 0; i < 16; ++i) {
        if (coord.x > end && edge.y > 0.8281 && edge.x == 0.0) {
            edge = smaaEdges(coord);
            coord -= vec2(2.0 * smaaMetrics.x, 0.0);
        }
    }
    float correction = -(255.0 / 127.0)
        * smaaSearchLength(edge, 0.0) + 3.25;
    return coord.x + smaaMetrics.x * correction;
}

float smaaSearchXRight(vec2 coord, float end)
{
    vec2 edge = vec2(0.0, 1.0);
    for (int i = 0; i < 16; ++i) {
        if (coord.x < end && edge.y > 0.8281 && edge.x == 0.0) {
            edge = smaaEdges(coord);
            coord += vec2(2.0 * smaaMetrics.x, 0.0);
        }
    }
    float correction = -(255.0 / 127.0)
        * smaaSearchLength(edge, 0.5) + 3.25;
    return coord.x - smaaMetrics.x * correction;
}

float smaaSearchYUp(vec2 coord, float end)
{
    vec2 edge = vec2(1.0, 0.0);
    for (int i = 0; i < 16; ++i) {
        if (coord.y > end && edge.x > 0.8281 && edge.y == 0.0) {
            edge = smaaEdges(coord);
            coord -= vec2(0.0, 2.0 * smaaMetrics.y);
        }
    }
    float correction = -(255.0 / 127.0)
        * smaaSearchLength(edge.yx, 0.0) + 3.25;
    return coord.y + smaaMetrics.y * correction;
}

float smaaSearchYDown(vec2 coord, float end)
{
    vec2 edge = vec2(1.0, 0.0);
    for (int i = 0; i < 16; ++i) {
        if (coord.y < end && edge.x > 0.8281 && edge.y == 0.0) {
            edge = smaaEdges(coord);
            coord += vec2(0.0, 2.0 * smaaMetrics.y);
        }
    }
    float correction = -(255.0 / 127.0)
        * smaaSearchLength(edge.yx, 0.5) + 3.25;
    return coord.y - smaaMetrics.y * correction;
}

vec2 smaaArea(vec2 distance, float crossing0, float crossing1)
{
    vec2 texelCoord = 16.0
        * round(4.0 * vec2(crossing0, crossing1)) + distance;
    vec2 uv = (texelCoord + vec2(0.5, 0.5)) / vec2(160.0, 560.0);
    return texture2D(areaTex, uv).rg;
}

vec2 smaaDecodeDiag(vec2 edge)
{
    edge.x = edge.x * abs(5.0 * edge.x - 3.75);
    return round(edge);
}

vec4 smaaSearchDiag1(vec2 coord, vec2 direction)
{
    float distance = -1.0;
    float continuation = 1.0;
    vec2 edge = vec2(0.0, 0.0);
    for (int i = 0; i < 8; ++i) {
        if (distance < 7.0 && continuation > 0.9) {
            coord += direction * smaaMetrics.xy;
            distance += 1.0;
            edge = smaaEdges(coord);
            continuation = dot(edge, vec2(0.5, 0.5));
        }
    }
    return vec4(distance, continuation, edge);
}

vec4 smaaSearchDiag2(vec2 coord, vec2 direction)
{
    float distance = -1.0;
    float continuation = 1.0;
    vec2 edge = vec2(0.0, 0.0);
    coord.x += 0.25 * smaaMetrics.x;
    for (int i = 0; i < 8; ++i) {
        if (distance < 7.0 && continuation > 0.9) {
            coord += direction * smaaMetrics.xy;
            distance += 1.0;
            edge = smaaDecodeDiag(smaaEdges(coord));
            continuation = dot(edge, vec2(0.5, 0.5));
        }
    }
    return vec4(distance, continuation, edge);
}

vec2 smaaAreaDiag(vec2 distance, vec2 crossing)
{
    vec2 texelCoord = 20.0 * crossing + distance;
    vec2 uv = (texelCoord + vec2(0.5, 0.5)) / vec2(160.0, 560.0);
    uv.x += 0.5;
    return texture2D(areaTex, uv).rg;
}

vec2 smaaCalculateDiagWeights(vec2 uv, vec2 edge)
{
    vec2 weights = vec2(0.0, 0.0);
    vec4 d = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 search;

    if (edge.x > 0.0) {
        search = smaaSearchDiag1(uv, vec2(-1.0, 1.0));
        d.x = search.x + (search.w > 0.9 ? 1.0 : 0.0);
        d.z = search.y;
    }
    search = smaaSearchDiag1(uv, vec2(1.0, -1.0));
    d.y = search.x;
    d.w = search.y;

    if (d.x + d.y > 2.0) {
        vec4 coords = vec4(-d.x + 0.25, d.x, d.y, -d.y - 0.25)
            * smaaMetrics.xyxy + uv.xyxy;
        vec4 crossing;
        crossing.xy = smaaEdgesOffset(coords.xy, vec2(-1.0, 0.0));
        crossing.zw = smaaEdgesOffset(coords.zw, vec2(1.0, 0.0));
        vec4 decoded = round(vec4(
            crossing.x * abs(5.0 * crossing.x - 3.75), crossing.y,
            crossing.z * abs(5.0 * crossing.z - 3.75), crossing.w));
        crossing = vec4(decoded.y, decoded.x, decoded.w, decoded.z);
        vec2 merged = 2.0 * crossing.xz + crossing.yw;
        merged *= vec2(1.0, 1.0) - step(vec2(0.9, 0.9), d.zw);
        weights += smaaAreaDiag(d.xy, merged);
    }

    search = smaaSearchDiag2(uv, vec2(-1.0, -1.0));
    d.x = search.x;
    d.z = search.y;
    if (smaaEdgesOffset(uv, vec2(1.0, 0.0)).x > 0.0) {
        search = smaaSearchDiag2(uv, vec2(1.0, 1.0));
        d.y = search.x + (search.w > 0.9 ? 1.0 : 0.0);
        d.w = search.y;
    } else {
        d.y = 0.0;
        d.w = 0.0;
    }

    if (d.x + d.y > 2.0) {
        vec4 coords = vec4(-d.x, -d.x, d.y, d.y)
            * smaaMetrics.xyxy + uv.xyxy;
        vec4 crossing;
        crossing.x = smaaEdgesOffset(coords.xy, vec2(-1.0, 0.0)).y;
        crossing.y = smaaEdgesOffset(coords.xy, vec2(0.0, -1.0)).x;
        vec2 farEdge = smaaEdgesOffset(coords.zw, vec2(1.0, 0.0));
        crossing.z = farEdge.y;
        crossing.w = farEdge.x;
        vec2 merged = 2.0 * crossing.xz + crossing.yw;
        merged *= vec2(1.0, 1.0) - step(vec2(0.9, 0.9), d.zw);
        weights += smaaAreaDiag(d.xy, merged).yx;
    }
    return weights;
}

vec2 smaaHorizontalCorner(vec2 weights, vec4 coords, vec2 distance)
{
    vec2 leftRight = step(distance.xy, distance.yx);
    vec2 rounding = 0.75 * leftRight
        / max(leftRight.x + leftRight.y, 1.0);
    vec2 factor = vec2(1.0, 1.0);
    factor.x -= rounding.x
        * smaaEdgesOffset(coords.xy, vec2(0.0, 1.0)).x;
    factor.x -= rounding.y
        * smaaEdgesOffset(coords.zw, vec2(1.0, 1.0)).x;
    factor.y -= rounding.x
        * smaaEdgesOffset(coords.xy, vec2(0.0, -2.0)).x;
    factor.y -= rounding.y
        * smaaEdgesOffset(coords.zw, vec2(1.0, -2.0)).x;
    return weights * clamp(factor, vec2(0.0, 0.0), vec2(1.0, 1.0));
}

vec2 smaaVerticalCorner(vec2 weights, vec4 coords, vec2 distance)
{
    vec2 topBottom = step(distance.xy, distance.yx);
    vec2 rounding = 0.75 * topBottom
        / max(topBottom.x + topBottom.y, 1.0);
    vec2 factor = vec2(1.0, 1.0);
    factor.x -= rounding.x
        * smaaEdgesOffset(coords.xy, vec2(1.0, 0.0)).y;
    factor.x -= rounding.y
        * smaaEdgesOffset(coords.zw, vec2(1.0, 1.0)).y;
    factor.y -= rounding.x
        * smaaEdgesOffset(coords.xy, vec2(-2.0, 0.0)).y;
    factor.y -= rounding.y
        * smaaEdgesOffset(coords.zw, vec2(-2.0, 1.0)).y;
    return weights * clamp(factor, vec2(0.0, 0.0), vec2(1.0, 1.0));
}

void main()
{
    vec2 uv = smaaClampUv(v_texcoord0);
    vec2 texel = smaaMetrics.xy;
    vec2 pixelCoord = uv * smaaMetrics.zw;
    vec4 offset0 = uv.xyxy
        + texel.xyxy * vec4(-0.25, -0.125, 1.25, -0.125);
    vec4 offset1 = uv.xyxy
        + texel.xyxy * vec4(-0.125, -0.25, -0.125, 1.25);
    vec4 offset2 = vec4(offset0.x, offset0.z, offset1.y, offset1.w)
        + smaaMetrics.xxyy * vec4(-32.0, 32.0, -32.0, 32.0);
    vec2 edge = smaaEdges(uv);
    vec4 weights = vec4(0.0, 0.0, 0.0, 0.0);

    if (edge.y > 0.0) {
        weights.rg = smaaCalculateDiagWeights(uv, edge);
        if (dot(weights.rg, vec2(1.0, 1.0)) < 0.00001) {
            vec3 coords;
            coords.x = smaaSearchXLeft(offset0.xy, offset2.x);
            coords.y = offset1.y;
            float crossing0 = smaaEdges(coords.xy).x;
            coords.z = smaaSearchXRight(offset0.zw, offset2.y);
            vec2 distance = abs(round(
                smaaMetrics.zz * coords.xz - pixelCoord.xx));
            float crossing1 = smaaEdgesOffset(
                coords.zy, vec2(1.0, 0.0)).x;
            weights.rg = smaaArea(sqrt(distance), crossing0, crossing1);
            coords.y = uv.y;
            weights.rg = smaaHorizontalCorner(
                weights.rg, vec4(coords.x, coords.y, coords.z, coords.y),
                distance);
        } else {
            edge.x = 0.0;
        }
    }

    if (edge.x > 0.0) {
        vec3 coords;
        coords.y = smaaSearchYUp(offset1.xy, offset2.z);
        coords.x = offset0.x;
        float crossing0 = smaaEdges(coords.xy).y;
        coords.z = smaaSearchYDown(offset1.zw, offset2.w);
        vec2 distance = abs(round(
            smaaMetrics.ww * coords.yz - pixelCoord.yy));
        float crossing1 = smaaEdgesOffset(
            coords.xz, vec2(0.0, 1.0)).y;
        weights.ba = smaaArea(sqrt(distance), crossing0, crossing1);
        coords.x = uv.x;
        weights.ba = smaaVerticalCorner(
            weights.ba, vec4(coords.x, coords.y, coords.x, coords.z),
            distance);
    }

    gl_FragColor = weights;
}
)";

constexpr const char* kSmaaNeighborhoodFragmentSc = R"(
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(inputColor, 0);
SAMPLER2D(blendTex, 1);
uniform vec4 smaaMetrics;

vec2 smaaClampUv(vec2 uv)
{
    return clamp(uv, vec2(0.0, 0.0), vec2(1.0, 1.0));
}

void main()
{
    vec2 uv = smaaClampUv(v_texcoord0);
    vec2 texel = smaaMetrics.xy;
    vec4 centerWeights = texture2D(blendTex, uv);
    vec4 a;
    a.x = texture2D(blendTex,
        smaaClampUv(uv + vec2(texel.x, 0.0))).a;
    a.y = texture2D(blendTex,
        smaaClampUv(uv + vec2(0.0, texel.y))).g;
    a.z = centerWeights.b;
    a.w = centerWeights.r;

    if (dot(a, vec4(1.0, 1.0, 1.0, 1.0)) < 0.00001) {
        gl_FragColor = texture2D(inputColor, uv);
        return;
    }

    bool horizontal = max(a.x, a.z) > max(a.y, a.w);
    vec2 coord0;
    vec2 coord1;
    vec2 blendWeight;
    if (horizontal) {
        coord0 = smaaClampUv(uv + vec2(a.x * texel.x, 0.0));
        coord1 = smaaClampUv(uv - vec2(a.z * texel.x, 0.0));
        blendWeight = a.xz;
    } else {
        coord0 = smaaClampUv(uv + vec2(0.0, a.y * texel.y));
        coord1 = smaaClampUv(uv - vec2(0.0, a.w * texel.y));
        blendWeight = a.yw;
    }
    blendWeight /= max(dot(blendWeight, vec2(1.0, 1.0)), 0.00001);
    gl_FragColor = blendWeight.x * texture2D(inputColor, coord0)
                 + blendWeight.y * texture2D(inputColor, coord1);
}
)";

constexpr const char* kSmaaEdgeCacheKey = "smaa1x_color_edge_v2";
constexpr const char* kSmaaWeightCacheKey = "smaa1x_high_weights_v3";
constexpr const char* kSmaaNeighborhoodCacheKey = "smaa1x_neighborhood_v2";

} // namespace

const char* const kSmaaEdgeCacheKeyCStr = kSmaaEdgeCacheKey;
const char* const kSmaaWeightCacheKeyCStr = kSmaaWeightCacheKey;
const char* const kSmaaNeighborhoodCacheKeyCStr = kSmaaNeighborhoodCacheKey;

const char* smaaVaryingScForTests() noexcept { return kSmaaVaryingSc; }
const char* smaaVertexScForTests() noexcept { return kSmaaVertexSc; }
const char* smaaEdgeFragmentScForTests() noexcept { return kSmaaEdgeFragmentSc; }
const char* smaaWeightFragmentScForTests() noexcept { return kSmaaWeightFragmentSc; }
const char* smaaNeighborhoodFragmentScForTests() noexcept
{
    return kSmaaNeighborhoodFragmentSc;
}

bool SMAAPass::isReady() const noexcept
{
    return _geometry.isReady()
        && _edgeProgram.isValid()
        && _weightProgram.isValid()
        && _neighborhoodProgram.isValid()
        && BGFXAdapter::isValid(_areaTexture)
        && BGFXAdapter::isValid(_searchTexture)
        && _edgeInputColor != ayt::shader::InvalidBinding
        && _edgeMetrics != ayt::shader::InvalidBinding
        && _edgeParams != ayt::shader::InvalidBinding
        && _weightEdges != ayt::shader::InvalidBinding
        && _weightArea != ayt::shader::InvalidBinding
        && _weightSearch != ayt::shader::InvalidBinding
        && _weightMetrics != ayt::shader::InvalidBinding
        && _neighborhoodColor != ayt::shader::InvalidBinding
        && _neighborhoodBlend != ayt::shader::InvalidBinding
        && _neighborhoodMetrics != ayt::shader::InvalidBinding;
}

uint32_t SMAAPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    if (!adapter.isInitialized() || adapter.isNoopBackend()
        || ctx.viewportWidth == 0 || ctx.viewportHeight == 0
        || ctx.frameGraph == nullptr
        || !ctx.frameGraph->semanticProducedThisFrame(FgSemantic::PresentSource)) {
        return 0;
    }

    const bgfx::FrameBufferHandle source =
        ctx.frameGraph->resolveSemantic(FgSemantic::PresentSource);
    const bgfx::FrameBufferHandle edges =
        ctx.frameGraph->resolve(FgResourceId::SmaaEdges);
    const bgfx::FrameBufferHandle blend =
        ctx.frameGraph->resolve(FgResourceId::SmaaBlendWeights);
    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::SmaaColor);
    if (!BGFXAdapter::isValid(source) || !BGFXAdapter::isValid(edges)
        || !BGFXAdapter::isValid(blend) || !BGFXAdapter::isValid(target)) {
        rateLimitedEarlyReturn("SMAAPass", "FrameGraph source/targets invalid");
        return 0;
    }

    const bgfx::TextureHandle sourceColor = adapter.getFboAttachment(source, 0);
    const bgfx::TextureHandle edgeTexture = adapter.getFboAttachment(edges, 0);
    const bgfx::TextureHandle blendTexture = adapter.getFboAttachment(blend, 0);
    if (!BGFXAdapter::isValid(sourceColor)
        || !BGFXAdapter::isValid(edgeTexture)
        || !BGFXAdapter::isValid(blendTexture)) {
        rateLimitedEarlyReturn("SMAAPass", "FrameGraph attachment invalid");
        return 0;
    }

    if (!_geometry.ensure(adapter) || !ensureLookupTextures(adapter)) {
        rateLimitedEarlyReturn("SMAAPass", "geometry/lookup textures unavailable");
        return 0;
    }
    ensurePrograms(ctx.pool);
    if (!isReady()) {
        rateLimitedEarlyReturn("SMAAPass", "program unavailable");
        return 0;
    }

    const float metrics[4] = {
        1.0f / static_cast<float>(ctx.viewportWidth),
        1.0f / static_cast<float>(ctx.viewportHeight),
        static_cast<float>(ctx.viewportWidth),
        static_cast<float>(ctx.viewportHeight),
    };
    const float params[4] = {
        kEdgeThreshold,
        kLocalContrastAdaptation,
        static_cast<float>(kMaxSearchSteps),
        kCornerRounding,
    };
    ayt::shader::DrawCallContext draw;
    draw.state = 0;

    configureFullscreenPassView(adapter, kEdgeViewId, edges, 0, 0,
                                ctx.viewportWidth, ctx.viewportHeight);
    _geometry.bind(adapter);
    _edgeProgram.setTexture(0, _edgeInputColor, toShaderTexture(sourceColor));
    _edgeProgram.setUniform(_edgeMetrics, metrics, sizeof(metrics));
    _edgeProgram.setUniform(_edgeParams, params, sizeof(params));
    adapter.setStateDepthTestAlways();
    draw.viewId = kEdgeViewId;
    _edgeProgram.submit(draw);
    ctx.frameGraph->markProduced(FgResourceId::SmaaEdges);

    configureFullscreenPassView(adapter, kBlendWeightViewId, blend, 0, 0,
                                ctx.viewportWidth, ctx.viewportHeight);
    _geometry.bind(adapter);
    _weightProgram.setTexture(0, _weightEdges, toShaderTexture(edgeTexture));
    _weightProgram.setTexture(1, _weightArea, toShaderTexture(_areaTexture));
    _weightProgram.setTexture(2, _weightSearch, toShaderTexture(_searchTexture));
    _weightProgram.setUniform(_weightMetrics, metrics, sizeof(metrics));
    adapter.setStateDepthTestAlways();
    draw.viewId = kBlendWeightViewId;
    _weightProgram.submit(draw);
    ctx.frameGraph->markProduced(FgResourceId::SmaaBlendWeights);

    configureFullscreenPassView(adapter, kNeighborhoodViewId, target, 0, 0,
                                ctx.viewportWidth, ctx.viewportHeight);
    _geometry.bind(adapter);
    _neighborhoodProgram.setTexture(0, _neighborhoodColor,
                                    toShaderTexture(sourceColor));
    _neighborhoodProgram.setTexture(1, _neighborhoodBlend,
                                    toShaderTexture(blendTexture));
    _neighborhoodProgram.setUniform(_neighborhoodMetrics,
                                    metrics, sizeof(metrics));
    adapter.setStateDepthTestAlways();
    draw.viewId = kNeighborhoodViewId;
    _neighborhoodProgram.submit(draw);

    ctx.frameGraph->markProduced(FgResourceId::SmaaColor);
    ctx.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,
                                        FgResourceId::SmaaColor);
    if (!_firstDispatchLogged) {
        std::fprintf(stderr,
                     "[SMAAPass] first SMAA 1x High dispatch views=%u/%u/%u "
                     "size=%ux%u threshold=%.2f search=%u diag=%u\n",
                     static_cast<unsigned>(kEdgeViewId),
                     static_cast<unsigned>(kBlendWeightViewId),
                     static_cast<unsigned>(kNeighborhoodViewId),
                     static_cast<unsigned>(ctx.viewportWidth),
                     static_cast<unsigned>(ctx.viewportHeight),
                     static_cast<double>(kEdgeThreshold),
                     static_cast<unsigned>(kMaxSearchSteps),
                     static_cast<unsigned>(kMaxDiagonalSearchSteps));
        _firstDispatchLogged = true;
    }
    return 3;
}

bool SMAAPass::ensureLookupTextures(BGFXAdapter& adapter)
{
    if (!BGFXAdapter::isValid(_areaTexture)) {
        const std::vector<uint8_t> pixels = generateSmaaAreaTextureRg8();
        _areaTexture = adapter.createTexture2DFromData(
            kAreaTextureWidth,
            kAreaTextureHeight,
            bgfx::TextureFormat::RG8,
            pixels.data(),
            static_cast<uint32_t>(pixels.size()),
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    }
    if (!BGFXAdapter::isValid(_searchTexture)) {
        const std::vector<uint8_t> pixels = generateSmaaSearchTextureR8();
        _searchTexture = adapter.createTexture2DFromData(
            kSearchTextureWidth,
            kSearchTextureHeight,
            bgfx::TextureFormat::R8,
            pixels.data(),
            static_cast<uint32_t>(pixels.size()),
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    }
    return BGFXAdapter::isValid(_areaTexture)
        && BGFXAdapter::isValid(_searchTexture);
}

void SMAAPass::ensurePrograms(shader::ShaderResourcePool& pool)
{
    if (_edgeProgram.isValid() && _weightProgram.isValid()
        && _neighborhoodProgram.isValid()) {
        return;
    }
    if (_programRetryFrames > 0) {
        --_programRetryFrames;
        return;
    }

    ayt::shader::ShaderResource edge = pool.acquireFromBgfxSc(
        kSmaaVertexSc, kSmaaEdgeFragmentSc, kSmaaVaryingSc,
        kSmaaEdgeCacheKey);
    ayt::shader::ShaderResource weight = pool.acquireFromBgfxSc(
        kSmaaVertexSc, kSmaaWeightFragmentSc, kSmaaVaryingSc,
        kSmaaWeightCacheKey);
    ayt::shader::ShaderResource neighborhood = pool.acquireFromBgfxSc(
        kSmaaVertexSc, kSmaaNeighborhoodFragmentSc, kSmaaVaryingSc,
        kSmaaNeighborhoodCacheKey);

    const auto textureBinding = [](ayt::shader::ShaderResource& program,
                                   const char* name) {
        return program.isValid() ? program.getTextureBinding(name)
                                 : ayt::shader::InvalidBinding;
    };
    const auto uniformBinding = [](ayt::shader::ShaderResource& program,
                                   const char* name) {
        return program.isValid() ? program.getUniformBinding(name)
                                 : ayt::shader::InvalidBinding;
    };

    const ayt::shader::BindingId edgeInput = textureBinding(edge, "inputColor");
    const ayt::shader::BindingId edgeMetrics = uniformBinding(edge, "smaaMetrics");
    const ayt::shader::BindingId edgeParams = uniformBinding(edge, "smaaParams");
    const ayt::shader::BindingId weightEdges = textureBinding(weight, "edgesTex");
    const ayt::shader::BindingId weightArea = textureBinding(weight, "areaTex");
    const ayt::shader::BindingId weightSearch = textureBinding(weight, "searchTex");
    const ayt::shader::BindingId weightMetrics = uniformBinding(weight, "smaaMetrics");
    const ayt::shader::BindingId neighborhoodColor =
        textureBinding(neighborhood, "inputColor");
    const ayt::shader::BindingId neighborhoodBlend =
        textureBinding(neighborhood, "blendTex");
    const ayt::shader::BindingId neighborhoodMetrics =
        uniformBinding(neighborhood, "smaaMetrics");

    if (!edge.isValid() || !weight.isValid() || !neighborhood.isValid()
        || edgeInput == ayt::shader::InvalidBinding
        || edgeMetrics == ayt::shader::InvalidBinding
        || edgeParams == ayt::shader::InvalidBinding
        || weightEdges == ayt::shader::InvalidBinding
        || weightArea == ayt::shader::InvalidBinding
        || weightSearch == ayt::shader::InvalidBinding
        || weightMetrics == ayt::shader::InvalidBinding
        || neighborhoodColor == ayt::shader::InvalidBinding
        || neighborhoodBlend == ayt::shader::InvalidBinding
        || neighborhoodMetrics == ayt::shader::InvalidBinding) {
        constexpr uint16_t kRetryIntervalFrames = 120;
        _programRetryFrames = kRetryIntervalFrames;
        std::fprintf(stderr,
                     "[SMAAPass] shader acquire/binding failed; retrying "
                     "in %u frames.\n",
                     static_cast<unsigned>(kRetryIntervalFrames));
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[SMAAPass]   %s\n", error.c_str());
        }
        return;
    }

    _edgeProgram = std::move(edge);
    _weightProgram = std::move(weight);
    _neighborhoodProgram = std::move(neighborhood);
    _edgeInputColor = edgeInput;
    _edgeMetrics = edgeMetrics;
    _edgeParams = edgeParams;
    _weightEdges = weightEdges;
    _weightArea = weightArea;
    _weightSearch = weightSearch;
    _weightMetrics = weightMetrics;
    _neighborhoodColor = neighborhoodColor;
    _neighborhoodBlend = neighborhoodBlend;
    _neighborhoodMetrics = neighborhoodMetrics;
    _programRetryFrames = 0;
}

void SMAAPass::destroyResources(BGFXAdapter& adapter)
{
    _geometry.destroy(adapter);
    if (BGFXAdapter::isValid(_areaTexture)) {
        adapter.destroy(_areaTexture);
    }
    if (BGFXAdapter::isValid(_searchTexture)) {
        adapter.destroy(_searchTexture);
    }
    _areaTexture = BGFX_INVALID_HANDLE;
    _searchTexture = BGFX_INVALID_HANDLE;
    _edgeProgram.reset();
    _weightProgram.reset();
    _neighborhoodProgram.reset();
    _edgeInputColor = ayt::shader::InvalidBinding;
    _edgeMetrics = ayt::shader::InvalidBinding;
    _edgeParams = ayt::shader::InvalidBinding;
    _weightEdges = ayt::shader::InvalidBinding;
    _weightArea = ayt::shader::InvalidBinding;
    _weightSearch = ayt::shader::InvalidBinding;
    _weightMetrics = ayt::shader::InvalidBinding;
    _neighborhoodColor = ayt::shader::InvalidBinding;
    _neighborhoodBlend = ayt::shader::InvalidBinding;
    _neighborhoodMetrics = ayt::shader::InvalidBinding;
    _programRetryFrames = 0;
    _firstDispatchLogged = false;
}

} // namespace ayt::render::detail
