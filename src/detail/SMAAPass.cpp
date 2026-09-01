#include "detail/SMAAPass.h"

#include "detail/FgResource.h"
#include "detail/GpuResources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace ayt::render::detail
{

namespace {

// Shader structure and the procedural Area lookup are derived from the SMAA
// reference implementation by Jorge Jimenez et al. (MIT). See
// THIRD_PARTY_NOTICES.md. SearchTex is intentionally unnecessary here: the
// weight shader performs exact one-pixel bounded searches rather than the
// reference implementation's optimized two-pixel search plus correction LUT.
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

float smaaLuma(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
    vec2 uv = smaaClampUv(v_texcoord0);
    vec2 texel = smaaMetrics.xy;
    float center = smaaLuma(texture2D(inputColor, uv).rgb);
    float left = smaaLuma(texture2D(inputColor,
        smaaClampUv(uv - vec2(texel.x, 0.0))).rgb);
    float top = smaaLuma(texture2D(inputColor,
        smaaClampUv(uv - vec2(0.0, texel.y))).rgb);

    vec2 delta = abs(center - vec2(left, top));
    vec2 edges = step(vec2(smaaParams.x, smaaParams.x), delta);
    if (dot(edges, vec2(1.0, 1.0)) == 0.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float right = smaaLuma(texture2D(inputColor,
        smaaClampUv(uv + vec2(texel.x, 0.0))).rgb);
    float bottom = smaaLuma(texture2D(inputColor,
        smaaClampUv(uv + vec2(0.0, texel.y))).rgb);
    vec2 oppositeDelta = abs(center - vec2(right, bottom));
    vec2 maxDelta = max(delta, oppositeDelta);

    float leftLeft = smaaLuma(texture2D(inputColor,
        smaaClampUv(uv - vec2(2.0 * texel.x, 0.0))).rgb);
    float topTop = smaaLuma(texture2D(inputColor,
        smaaClampUv(uv - vec2(0.0, 2.0 * texel.y))).rgb);
    vec2 secondDelta = abs(vec2(left, top) - vec2(leftLeft, topTop));
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
uniform vec4 smaaMetrics;

vec2 smaaClampUv(vec2 uv)
{
    return clamp(uv, vec2(0.0, 0.0), vec2(1.0, 1.0));
}

vec2 smaaEdges(vec2 uv)
{
    return texture2D(edgesTex, smaaClampUv(uv)).rg;
}

vec2 smaaArea(vec2 distancePixels, float crossing0, float crossing1)
{
    vec2 crossing = floor(4.0 * vec2(crossing0, crossing1) + 0.5);
    vec2 texelCoord = 16.0 * crossing +
        sqrt(clamp(distancePixels, vec2(0.0, 0.0), vec2(225.0, 225.0)));
    vec2 uv = (texelCoord + vec2(0.5, 0.5)) / 80.0;
    return texture2D(areaTex, uv).rg;
}

void main()
{
    vec2 uv = smaaClampUv(v_texcoord0);
    vec2 texel = smaaMetrics.xy;
    vec2 edge = smaaEdges(uv);
    vec4 weights = vec4(0.0, 0.0, 0.0, 0.0);

    if (edge.y > 0.0) {
        float leftDistance = 0.0;
        float rightDistance = 0.0;
        float leftCrossing = 0.0;
        float rightCrossing = 0.0;
        vec2 leftEndUv = uv;
        vec2 rightEndUv = uv;
        bool leftDone = false;
        bool rightDone = false;
        for (int i = 1; i <= 16; ++i) {
            float distance = float(i);
            if (!leftDone) {
                leftEndUv = uv - vec2(distance * texel.x, 0.0);
                vec2 sampleEdge = smaaEdges(leftEndUv);
                leftDistance = distance;
                if (sampleEdge.y < 0.5 || sampleEdge.x > 0.0) {
                    leftDone = true;
                }
            }
            if (!rightDone) {
                rightEndUv = uv + vec2(distance * texel.x, 0.0);
                vec2 sampleEdge = smaaEdges(rightEndUv);
                rightDistance = distance;
                if (sampleEdge.y < 0.5 || sampleEdge.x > 0.0) {
                    rightDone = true;
                }
            }
        }

        // SMAA's crossing offset is essential here. Sampling a quarter pixel
        // towards the north lets linear filtering distinguish the two
        // possible endpoint edges (0.25/0.75) instead of collapsing every
        // staircase to a null 0/4 AreaTex pattern.
        vec2 crossingOffset = vec2(0.0, -0.25 * texel.y);
        leftCrossing = smaaEdges(leftEndUv + crossingOffset).x;
        rightCrossing = smaaEdges(rightEndUv + crossingOffset).x;
        weights.rg = smaaArea(vec2(leftDistance, rightDistance),
                              leftCrossing, rightCrossing);
        float cornerAttenuation = 1.0
            - 0.25 * max(leftCrossing, rightCrossing);
        weights.rg *= cornerAttenuation;
    }

    if (edge.x > 0.0) {
        float topDistance = 0.0;
        float bottomDistance = 0.0;
        float topCrossing = 0.0;
        float bottomCrossing = 0.0;
        vec2 topEndUv = uv;
        vec2 bottomEndUv = uv;
        bool topDone = false;
        bool bottomDone = false;
        for (int i = 1; i <= 16; ++i) {
            float distance = float(i);
            if (!topDone) {
                topEndUv = uv - vec2(0.0, distance * texel.y);
                vec2 sampleEdge = smaaEdges(topEndUv);
                topDistance = distance;
                if (sampleEdge.x < 0.5 || sampleEdge.y > 0.0) {
                    topDone = true;
                }
            }
            if (!bottomDone) {
                bottomEndUv = uv + vec2(0.0, distance * texel.y);
                vec2 sampleEdge = smaaEdges(bottomEndUv);
                bottomDistance = distance;
                if (sampleEdge.x < 0.5 || sampleEdge.y > 0.0) {
                    bottomDone = true;
                }
            }
        }
        vec2 crossingOffset = vec2(-0.25 * texel.x, 0.0);
        topCrossing = smaaEdges(topEndUv + crossingOffset).y;
        bottomCrossing = smaaEdges(bottomEndUv + crossingOffset).y;
        weights.ba = smaaArea(vec2(topDistance, bottomDistance),
                              topCrossing, bottomCrossing);
        float cornerAttenuation = 1.0
            - 0.25 * max(topCrossing, bottomCrossing);
        weights.ba *= cornerAttenuation;
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
        smaaClampUv(uv - vec2(0.0, texel.y))).g;
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
        coord0 = smaaClampUv(uv - vec2(0.0, a.y * texel.y));
        coord1 = smaaClampUv(uv + vec2(0.0, a.w * texel.y));
        blendWeight = a.yw;
    }
    blendWeight /= max(dot(blendWeight, vec2(1.0, 1.0)), 0.00001);
    gl_FragColor = blendWeight.x * texture2D(inputColor, coord0)
                 + blendWeight.y * texture2D(inputColor, coord1);
}
)";

constexpr const char* kSmaaEdgeCacheKey = "smaa1x_luma_edge_v1";
constexpr const char* kSmaaWeightCacheKey = "smaa1x_ortho_weights_v2";
constexpr const char* kSmaaNeighborhoodCacheKey = "smaa1x_neighborhood_v1";

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }

Vec2 lerp(Vec2 a, Vec2 b, float t)
{
    return a + (b + a * -1.0f) * t;
}

Vec2 lineArea(Vec2 p1, Vec2 p2, int pixel)
{
    const Vec2 d{p2.x - p1.x, p2.y - p1.y};
    const float x1 = static_cast<float>(pixel);
    const float x2 = x1 + 1.0f;
    const float y1 = p1.y + d.y * (x1 - p1.x) / d.x;
    const float y2 = p1.y + d.y * (x2 - p1.x) / d.x;
    const bool inside = (x1 >= p1.x && x1 < p2.x)
        || (x2 > p1.x && x2 <= p2.x);
    if (!inside) {
        return {};
    }

    const bool trapezoid = std::signbit(y1) == std::signbit(y2)
        || std::abs(y1) < 0.0001f || std::abs(y2) < 0.0001f;
    if (trapezoid) {
        const float area = 0.5f * (y1 + y2);
        return area < 0.0f ? Vec2{std::abs(area), 0.0f}
                           : Vec2{0.0f, std::abs(area)};
    }

    const float crossing = -p1.y * d.x / d.y + p1.x;
    float integral = 0.0f;
    const float fraction = std::modf(crossing, &integral);
    const float a1 = crossing > p1.x ? y1 * fraction * 0.5f : 0.0f;
    const float a2 = crossing < p2.x
        ? y2 * (1.0f - fraction) * 0.5f : 0.0f;
    const float dominant = std::abs(a1) > std::abs(a2) ? a1 : -a2;
    return dominant < 0.0f
        ? Vec2{std::abs(a1), std::abs(a2)}
        : Vec2{std::abs(a2), std::abs(a1)};
}

std::pair<Vec2, Vec2> smoothArea(float distance, Vec2 a1, Vec2 a2)
{
    const Vec2 b1{0.5f * std::sqrt(2.0f * a1.x),
                  0.5f * std::sqrt(2.0f * a1.y)};
    const Vec2 b2{0.5f * std::sqrt(2.0f * a2.x),
                  0.5f * std::sqrt(2.0f * a2.y)};
    const float t = std::clamp(distance / 32.0f, 0.0f, 1.0f);
    return {lerp(b1, a1, t), lerp(b2, a2, t)};
}

Vec2 areaOrtho(uint8_t pattern, int left, int right)
{
    const float distance = static_cast<float>(left + right + 1);
    constexpr float upper = 0.5f;
    constexpr float lower = -0.5f;
    const Vec2 leftLower{0.0f, lower};
    const Vec2 leftUpper{0.0f, upper};
    const Vec2 middle{distance * 0.5f, 0.0f};
    const Vec2 rightLower{distance, lower};
    const Vec2 rightUpper{distance, upper};

    switch (pattern) {
    case 0: return {};
    case 1: return left <= right ? lineArea(leftLower, middle, left) : Vec2{};
    case 2: return left >= right ? lineArea(middle, rightLower, left) : Vec2{};
    case 3: {
        const Vec2 a1 = lineArea(leftLower, middle, left);
        const Vec2 a2 = lineArea(middle, rightLower, left);
        const auto smoothed = smoothArea(distance, a1, a2);
        return smoothed.first + smoothed.second;
    }
    case 4: return left <= right ? lineArea(leftUpper, middle, left) : Vec2{};
    case 5: return {};
    case 6: return lineArea(leftUpper, rightLower, left);
    case 7: return lineArea(leftUpper, rightLower, left);
    case 8: return left >= right ? lineArea(middle, rightUpper, left) : Vec2{};
    case 9: return lineArea(leftLower, rightUpper, left);
    case 10: return {};
    case 11: return lineArea(leftLower, rightUpper, left);
    case 12: {
        const Vec2 a1 = lineArea(leftUpper, middle, left);
        const Vec2 a2 = lineArea(middle, rightUpper, left);
        const auto smoothed = smoothArea(distance, a1, a2);
        return smoothed.first + smoothed.second;
    }
    case 13: return lineArea(leftLower, rightUpper, left);
    case 14: return lineArea(leftUpper, rightLower, left);
    case 15: return {};
    default: return {};
    }
}

uint8_t toAreaByte(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return static_cast<uint8_t>(value * 255.0f);
}

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

std::vector<uint8_t> generateSmaaAreaTextureRg8()
{
    constexpr std::array<std::array<uint8_t, 2>, 16> edgeLayout = {{
        {{0, 0}}, {{3, 0}}, {{0, 3}}, {{3, 3}},
        {{1, 0}}, {{4, 0}}, {{1, 3}}, {{4, 3}},
        {{0, 1}}, {{3, 1}}, {{0, 4}}, {{3, 4}},
        {{1, 1}}, {{4, 1}}, {{1, 4}}, {{4, 4}},
    }};
    constexpr uint16_t size = SMAAPass::kAreaTextureSize;
    constexpr uint16_t blockSize = 16;
    std::vector<uint8_t> pixels(size * size * 2u, 0u);
    for (uint8_t pattern = 0; pattern < edgeLayout.size(); ++pattern) {
        for (uint16_t y = 0; y < blockSize; ++y) {
            for (uint16_t x = 0; x < blockSize; ++x) {
                const Vec2 area = areaOrtho(pattern,
                                            static_cast<int>(x * x),
                                            static_cast<int>(y * y));
                const uint16_t dstX = edgeLayout[pattern][0] * blockSize + x;
                const uint16_t dstY = edgeLayout[pattern][1] * blockSize + y;
                const size_t index =
                    (static_cast<size_t>(dstY) * size + dstX) * 2u;
                pixels[index + 0u] = toAreaByte(area.x);
                pixels[index + 1u] = toAreaByte(area.y);
            }
        }
    }
    return pixels;
}

bool SMAAPass::isReady() const noexcept
{
    return _geometry.isReady()
        && _edgeProgram.isValid()
        && _weightProgram.isValid()
        && _neighborhoodProgram.isValid()
        && BGFXAdapter::isValid(_areaTexture)
        && _edgeInputColor != ayt::shader::InvalidBinding
        && _edgeMetrics != ayt::shader::InvalidBinding
        && _edgeParams != ayt::shader::InvalidBinding
        && _weightEdges != ayt::shader::InvalidBinding
        && _weightArea != ayt::shader::InvalidBinding
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

    if (!_geometry.ensure(adapter) || !ensureAreaTexture(adapter)) {
        rateLimitedEarlyReturn("SMAAPass", "geometry/AreaTex unavailable");
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
                     "[SMAAPass] first SMAA 1x dispatch views=%u/%u/%u "
                     "size=%ux%u threshold=%.2f search=%u\n",
                     static_cast<unsigned>(kEdgeViewId),
                     static_cast<unsigned>(kBlendWeightViewId),
                     static_cast<unsigned>(kNeighborhoodViewId),
                     static_cast<unsigned>(ctx.viewportWidth),
                     static_cast<unsigned>(ctx.viewportHeight),
                     static_cast<double>(kEdgeThreshold),
                     static_cast<unsigned>(kMaxSearchSteps));
        _firstDispatchLogged = true;
    }
    return 3;
}

bool SMAAPass::ensureAreaTexture(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_areaTexture)) {
        return true;
    }
    const std::vector<uint8_t> pixels = generateSmaaAreaTextureRg8();
    _areaTexture = adapter.createTexture2DFromData(
        kAreaTextureSize,
        kAreaTextureSize,
        bgfx::TextureFormat::RG8,
        pixels.data(),
        static_cast<uint32_t>(pixels.size()),
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    return BGFXAdapter::isValid(_areaTexture);
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
    _areaTexture = BGFX_INVALID_HANDLE;
    _edgeProgram.reset();
    _weightProgram.reset();
    _neighborhoodProgram.reset();
    _edgeInputColor = ayt::shader::InvalidBinding;
    _edgeMetrics = ayt::shader::InvalidBinding;
    _edgeParams = ayt::shader::InvalidBinding;
    _weightEdges = ayt::shader::InvalidBinding;
    _weightArea = ayt::shader::InvalidBinding;
    _weightMetrics = ayt::shader::InvalidBinding;
    _neighborhoodColor = ayt::shader::InvalidBinding;
    _neighborhoodBlend = ayt::shader::InvalidBinding;
    _neighborhoodMetrics = ayt::shader::InvalidBinding;
    _programRetryFrames = 0;
    _firstDispatchLogged = false;
}

} // namespace ayt::render::detail
