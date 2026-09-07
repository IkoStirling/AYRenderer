#include "detail/FXAAPass.h"

#include "detail/FgResource.h"
#include "detail/GpuResources.h"

#include <cstdio>
#include <string>
#include <utility>

namespace ayt::render::detail
{

namespace {

// Controlled raw-bgfx compatibility exception. Phoskia now emits `if`, but
// this quality FXAA still depends on reusable helper functions and a real
// early return for flat pixels. Material `return` currently assigns the bgfx
// output slot instead of terminating control flow (required by shaderc's D3D
// output wrapper), so translating this source today would change its meaning.
// Keep the exception local until Phoskia has user functions and a portable
// single-exit/control-flow lowering validated on both D3D backends.
constexpr const char* kFxaaVaryingSc = R"(
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 a_position  : POSITION;
vec2 a_texcoord0 : TEXCOORD0;
)";

constexpr const char* kFxaaVertexSc = R"(
$input a_position, a_texcoord0
$output v_texcoord0
#include <bgfx_shader.sh>
void main()
{
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
}
)";

// Display-referred quality FXAA. Five cardinal samples establish a contrast
// gate first, so flat/low-contrast pixels preserve the exact center value.
// Confirmed edges fetch the corners for orientation, then search at bounded
// 1/2/4/8-pixel distances along the edge. The final sample moves only across
// the edge normal; it does not average a broad directional kernel.
constexpr const char* kFxaaFragmentSc = R"(
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(inputColor, 0);
uniform vec4 inverseViewport;
uniform vec4 fxaaQuality;

float fxaaLuma(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

float fxaaColorDistance(vec3 a, vec3 b)
{
    vec3 delta = abs(a - b);
    return max(max(delta.r, delta.g), delta.b);
}

float fxaaSecondDerivative(vec3 center, vec3 a, vec3 b)
{
    vec3 delta = abs(-2.0 * center + a + b);
    return max(max(delta.r, delta.g), delta.b);
}

vec2 fxaaClampUv(vec2 uv)
{
    return clamp(uv, vec2(0.0, 0.0), vec2(1.0, 1.0));
}

void main()
{
    vec2 uv = fxaaClampUv(v_texcoord0);
    vec2 texel = inverseViewport.xy;
    vec4 colorM = texture2D(inputColor, uv);
    float lumaM = fxaaLuma(colorM.rgb);

    vec2 uvN = fxaaClampUv(uv + vec2(0.0, -texel.y));
    vec2 uvE = fxaaClampUv(uv + vec2(texel.x, 0.0));
    vec2 uvS = fxaaClampUv(uv + vec2(0.0, texel.y));
    vec2 uvW = fxaaClampUv(uv + vec2(-texel.x, 0.0));
    vec3 colorN = texture2D(inputColor, uvN).rgb;
    vec3 colorE = texture2D(inputColor, uvE).rgb;
    vec3 colorS = texture2D(inputColor, uvS).rgb;
    vec3 colorW = texture2D(inputColor, uvW).rgb;
    float lumaN = fxaaLuma(colorN);
    float lumaE = fxaaLuma(colorE);
    float lumaS = fxaaLuma(colorS);
    float lumaW = fxaaLuma(colorW);

    float lumaMin = min(lumaM, min(min(lumaN, lumaE), min(lumaS, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaE), max(lumaS, lumaW)));
    float lumaRange = lumaMax - lumaMin;
    float colorRange = max(
        max(fxaaColorDistance(colorM.rgb, colorN),
            fxaaColorDistance(colorM.rgb, colorE)),
        max(fxaaColorDistance(colorM.rgb, colorS),
            fxaaColorDistance(colorM.rgb, colorW)));
    float edgeRange = max(lumaRange, colorRange);
    float edgeThreshold = max(fxaaQuality.y, lumaMax * fxaaQuality.x);
    if (edgeRange < edgeThreshold) {
        gl_FragColor = colorM;
        return;
    }

    vec2 uvNW = fxaaClampUv(uv + vec2(-texel.x, -texel.y));
    vec2 uvNE = fxaaClampUv(uv + vec2( texel.x, -texel.y));
    vec2 uvSW = fxaaClampUv(uv + vec2(-texel.x,  texel.y));
    vec2 uvSE = fxaaClampUv(uv + vec2( texel.x,  texel.y));
    vec3 colorNW = texture2D(inputColor, uvNW).rgb;
    vec3 colorNE = texture2D(inputColor, uvNE).rgb;
    vec3 colorSW = texture2D(inputColor, uvSW).rgb;
    vec3 colorSE = texture2D(inputColor, uvSE).rgb;
    float lumaNW = fxaaLuma(colorNW);
    float lumaNE = fxaaLuma(colorNE);
    float lumaSW = fxaaLuma(colorSW);
    float lumaSE = fxaaLuma(colorSE);

    float edgeHorizontal =
          abs(-2.0 * lumaM + lumaN + lumaS) * 2.0
        + abs(-2.0 * lumaE + lumaNE + lumaSE)
        + abs(-2.0 * lumaW + lumaNW + lumaSW);
    float edgeVertical =
          abs(-2.0 * lumaM + lumaE + lumaW) * 2.0
        + abs(-2.0 * lumaN + lumaNE + lumaNW)
        + abs(-2.0 * lumaS + lumaSE + lumaSW);
    // Preserve hue-only edges too. The luma terms retain canonical FXAA
    // behavior; RGB derivatives only win when a saturated edge is otherwise
    // close to isoluminant.
    edgeHorizontal = max(edgeHorizontal,
          fxaaSecondDerivative(colorM.rgb, colorN, colorS) * 2.0
        + fxaaSecondDerivative(colorE, colorNE, colorSE)
        + fxaaSecondDerivative(colorW, colorNW, colorSW));
    edgeVertical = max(edgeVertical,
          fxaaSecondDerivative(colorM.rgb, colorE, colorW) * 2.0
        + fxaaSecondDerivative(colorN, colorNE, colorNW)
        + fxaaSecondDerivative(colorS, colorSE, colorSW));
    bool isHorizontal = edgeHorizontal >= edgeVertical;

    float lumaNegative = isHorizontal ? lumaN : lumaW;
    float lumaPositive = isHorizontal ? lumaS : lumaE;
    vec3 colorNegative = isHorizontal ? colorN : colorW;
    vec3 colorPositive = isHorizontal ? colorS : colorE;
    float gradientNegative = max(abs(lumaNegative - lumaM),
        fxaaColorDistance(colorNegative, colorM.rgb));
    float gradientPositive = max(abs(lumaPositive - lumaM),
        fxaaColorDistance(colorPositive, colorM.rgb));
    bool useNegative = gradientNegative >= gradientPositive;
    float gradient = max(gradientNegative, gradientPositive);
    float lumaLocalAverage = 0.5 *
        (lumaM + (useNegative ? lumaNegative : lumaPositive));
    vec3 colorLocalAverage = 0.5 *
        (colorM.rgb + (useNegative ? colorNegative : colorPositive));

    vec2 normalStep = isHorizontal
        ? vec2(0.0, texel.y)
        : vec2(texel.x, 0.0);
    normalStep *= useNegative ? -1.0 : 1.0;
    vec2 tangentStep = isHorizontal
        ? vec2(texel.x, 0.0)
        : vec2(0.0, texel.y);
    vec2 edgeUv = fxaaClampUv(uv + normalStep * 0.5);
    float gradientThreshold = gradient * fxaaQuality.w;

    vec2 uvNegative = fxaaClampUv(edgeUv - tangentStep);
    vec2 uvPositive = fxaaClampUv(edgeUv + tangentStep);
    vec3 endpointColorNegative = texture2D(inputColor, uvNegative).rgb;
    vec3 endpointColorPositive = texture2D(inputColor, uvPositive).rgb;
    float deltaNegative = fxaaLuma(endpointColorNegative) - lumaLocalAverage;
    float deltaPositive = fxaaLuma(endpointColorPositive) - lumaLocalAverage;
    float colorDeltaNegative = fxaaColorDistance(
        endpointColorNegative, colorLocalAverage);
    float colorDeltaPositive = fxaaColorDistance(
        endpointColorPositive, colorLocalAverage);
    bool doneNegative = max(abs(deltaNegative), colorDeltaNegative)
        >= gradientThreshold;
    bool donePositive = max(abs(deltaPositive), colorDeltaPositive)
        >= gradientThreshold;
    float distanceNegative = 1.0;
    float distancePositive = 1.0;

    if (!doneNegative) {
        uvNegative = fxaaClampUv(uvNegative - tangentStep);
        distanceNegative = 2.0;
        endpointColorNegative = texture2D(inputColor, uvNegative).rgb;
        deltaNegative = fxaaLuma(endpointColorNegative) - lumaLocalAverage;
        colorDeltaNegative = fxaaColorDistance(
            endpointColorNegative, colorLocalAverage);
        doneNegative = max(abs(deltaNegative), colorDeltaNegative)
            >= gradientThreshold;
    }
    if (!donePositive) {
        uvPositive = fxaaClampUv(uvPositive + tangentStep);
        distancePositive = 2.0;
        endpointColorPositive = texture2D(inputColor, uvPositive).rgb;
        deltaPositive = fxaaLuma(endpointColorPositive) - lumaLocalAverage;
        colorDeltaPositive = fxaaColorDistance(
            endpointColorPositive, colorLocalAverage);
        donePositive = max(abs(deltaPositive), colorDeltaPositive)
            >= gradientThreshold;
    }
    if (!doneNegative) {
        uvNegative = fxaaClampUv(uvNegative - tangentStep * 2.0);
        distanceNegative = 4.0;
        endpointColorNegative = texture2D(inputColor, uvNegative).rgb;
        deltaNegative = fxaaLuma(endpointColorNegative) - lumaLocalAverage;
        colorDeltaNegative = fxaaColorDistance(
            endpointColorNegative, colorLocalAverage);
        doneNegative = max(abs(deltaNegative), colorDeltaNegative)
            >= gradientThreshold;
    }
    if (!donePositive) {
        uvPositive = fxaaClampUv(uvPositive + tangentStep * 2.0);
        distancePositive = 4.0;
        endpointColorPositive = texture2D(inputColor, uvPositive).rgb;
        deltaPositive = fxaaLuma(endpointColorPositive) - lumaLocalAverage;
        colorDeltaPositive = fxaaColorDistance(
            endpointColorPositive, colorLocalAverage);
        donePositive = max(abs(deltaPositive), colorDeltaPositive)
            >= gradientThreshold;
    }
    if (!doneNegative) {
        uvNegative = fxaaClampUv(uvNegative - tangentStep * 4.0);
        distanceNegative = 8.0;
        deltaNegative =
            fxaaLuma(texture2D(inputColor, uvNegative).rgb) - lumaLocalAverage;
    }
    if (!donePositive) {
        uvPositive = fxaaClampUv(uvPositive + tangentStep * 4.0);
        distancePositive = 8.0;
        deltaPositive =
            fxaaLuma(texture2D(inputColor, uvPositive).rgb) - lumaLocalAverage;
    }

    bool negativeNearest = distanceNegative < distancePositive;
    float endpointDelta = negativeNearest ? deltaNegative : deltaPositive;
    bool centerIsDarker = lumaM < lumaLocalAverage;
    bool endpointIsDarker = endpointDelta < 0.0;
    bool chromaDominant = colorRange > lumaRange;
    bool correctVariation = chromaDominant
        || endpointIsDarker != centerIsDarker;
    float nearestDistance = min(distanceNegative, distancePositive);
    float edgeOffset = 0.5 - nearestDistance /
        max(distanceNegative + distancePositive, 0.0001);
    if (!correctVariation) {
        edgeOffset = 0.0;
    }

    float lumaAverage = (2.0 * (lumaN + lumaE + lumaS + lumaW)
        + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    vec3 colorAverage = (2.0 * (colorN + colorE + colorS + colorW)
        + colorNW + colorNE + colorSW + colorSE) / 12.0;
    float subpixelDelta = max(abs(lumaAverage - lumaM),
        fxaaColorDistance(colorAverage, colorM.rgb));
    float subpixel = clamp(subpixelDelta /
        max(edgeRange, 0.0001), 0.0, 1.0);
    subpixel = smoothstep(0.0, 1.0, subpixel);
    float subpixelOffset = subpixel * subpixel * fxaaQuality.z;

    float finalOffset = max(edgeOffset, subpixelOffset);
    vec2 finalUv = fxaaClampUv(uv + normalStep * finalOffset);
    vec3 filtered = texture2D(inputColor, finalUv).rgb;
    gl_FragColor = vec4(filtered, colorM.a);
}
)";

constexpr const char* kFxaaCacheKey = "fxaa_chroma_edge_search_v3";

} // namespace

const char* const kFxaaCacheKeyCStr = kFxaaCacheKey;

const char* fxaaVaryingScForTests() noexcept
{
    return kFxaaVaryingSc;
}

const char* fxaaVertexScForTests() noexcept
{
    return kFxaaVertexSc;
}

const char* fxaaFragmentScForTests() noexcept
{
    return kFxaaFragmentSc;
}

uint32_t FXAAPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    if (!adapter.isInitialized() || adapter.isNoopBackend()
        || ctx.viewportWidth == 0 || ctx.viewportHeight == 0
        || ctx.frameGraph == nullptr
        || !ctx.frameGraph->producedThisFrame(FgResourceId::FinalLdrColor)) {
        return 0;
    }

    const bgfx::FrameBufferHandle source =
        ctx.frameGraph->resolve(FgResourceId::FinalLdrColor);
    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::FxaaColor);
    if (!BGFXAdapter::isValid(source) || !BGFXAdapter::isValid(target)) {
        rateLimitedEarlyReturn("FXAAPass", "FrameGraph source/target invalid");
        return 0;
    }
    const bgfx::TextureHandle color = adapter.getFboAttachment(source, 0);
    if (!BGFXAdapter::isValid(color)) {
        rateLimitedEarlyReturn("FXAAPass", "FinalLdrColor attachment invalid");
        return 0;
    }

    if (!_geometry.ensure(adapter)) {
        rateLimitedEarlyReturn("FXAAPass", "fullscreen geometry invalid");
        return 0;
    }
    ensureProgram(ctx.pool);
    if (!isReady()) {
        rateLimitedEarlyReturn("FXAAPass", "program unavailable");
        return 0;
    }

    configureFullscreenPassView(adapter,
                                kFxaaViewId,
                                target,
                                0,
                                0,
                                ctx.viewportWidth,
                                ctx.viewportHeight);
    _geometry.bind(adapter);
    _program.setTexture(0, _tInputColor, toShaderTexture(color));
    const float inverseViewport[4] = {
        1.0f / static_cast<float>(ctx.viewportWidth),
        1.0f / static_cast<float>(ctx.viewportHeight),
        static_cast<float>(ctx.viewportWidth),
        static_cast<float>(ctx.viewportHeight),
    };
    _program.setUniform(_uInverseViewport,
                        inverseViewport,
                        sizeof(inverseViewport));
    const float quality[4] = {
        kEdgeThreshold,
        kEdgeThresholdMin,
        kSubpixelQuality,
        kSearchThreshold,
    };
    _program.setUniform(_uFxaaQuality, quality, sizeof(quality));
    adapter.setStateDepthTestAlways();

    ayt::shader::DrawCallContext draw;
    draw.viewId = kFxaaViewId;
    draw.state = 0;
    _program.submit(draw);

    // Promotion is transactional: PresentSource remains FinalLdrColor until
    // the filtered draw is known to have been submitted this frame.
    ctx.frameGraph->markProduced(FgResourceId::FxaaColor);
    ctx.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,
                                        FgResourceId::FxaaColor);
    return 1;
}

void FXAAPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    if (_program.isValid()) {
        return;
    }
    if (_programRetryFrames > 0) {
        --_programRetryFrames;
        return;
    }

    ayt::shader::ShaderResource program = pool.acquireFromBgfxSc(
        kFxaaVertexSc,
        kFxaaFragmentSc,
        kFxaaVaryingSc,
        kFxaaCacheKey);
    const ayt::shader::BindingId inputColor = program.isValid()
        ? program.getTextureBinding("inputColor")
        : ayt::shader::InvalidBinding;
    const ayt::shader::BindingId inverseViewport = program.isValid()
        ? program.getUniformBinding("inverseViewport")
        : ayt::shader::InvalidBinding;
    const ayt::shader::BindingId fxaaQuality = program.isValid()
        ? program.getUniformBinding("fxaaQuality")
        : ayt::shader::InvalidBinding;
    if (!program.isValid()
        || inputColor == ayt::shader::InvalidBinding
        || inverseViewport == ayt::shader::InvalidBinding
        || fxaaQuality == ayt::shader::InvalidBinding) {
        constexpr uint16_t kRetryIntervalFrames = 120;
        _programRetryFrames = kRetryIntervalFrames;
        std::fprintf(stderr,
                     "[FXAAPass] shader acquire/binding failed; retrying "
                     "in %u frames.\n",
                     static_cast<unsigned>(kRetryIntervalFrames));
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[FXAAPass]   %s\n", error.c_str());
        }
        return;
    }

    _program = std::move(program);
    _tInputColor = inputColor;
    _uInverseViewport = inverseViewport;
    _uFxaaQuality = fxaaQuality;
    _programRetryFrames = 0;
}

void FXAAPass::destroyResources(BGFXAdapter& adapter)
{
    _geometry.destroy(adapter);
    _program.reset();
    _tInputColor = ayt::shader::InvalidBinding;
    _uInverseViewport = ayt::shader::InvalidBinding;
    _uFxaaQuality = ayt::shader::InvalidBinding;
    _programRetryFrames = 0;
}

} // namespace ayt::render::detail
