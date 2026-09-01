#include "detail/FXAAPass.h"

#include "detail/FgResource.h"
#include "detail/GpuResources.h"

#include <cstdio>
#include <string>
#include <utility>

namespace ayt::render::detail
{

namespace {

// Phoskia currently lowers IfStmt into IR but the BGFX backend intentionally
// omits control-flow emission. Quality FXAA needs a real early-out for flat
// pixels and a bounded endpoint search, so this pass uses the engine's existing
// raw bgfx .sc path (also used by alpha-cutout GBuffer and editor overlays).
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
    float lumaN = fxaaLuma(texture2D(inputColor, uvN).rgb);
    float lumaE = fxaaLuma(texture2D(inputColor, uvE).rgb);
    float lumaS = fxaaLuma(texture2D(inputColor, uvS).rgb);
    float lumaW = fxaaLuma(texture2D(inputColor, uvW).rgb);

    float lumaMin = min(lumaM, min(min(lumaN, lumaE), min(lumaS, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaE), max(lumaS, lumaW)));
    float lumaRange = lumaMax - lumaMin;
    float edgeThreshold = max(fxaaQuality.y, lumaMax * fxaaQuality.x);
    if (lumaRange < edgeThreshold) {
        gl_FragColor = colorM;
        return;
    }

    vec2 uvNW = fxaaClampUv(uv + vec2(-texel.x, -texel.y));
    vec2 uvNE = fxaaClampUv(uv + vec2( texel.x, -texel.y));
    vec2 uvSW = fxaaClampUv(uv + vec2(-texel.x,  texel.y));
    vec2 uvSE = fxaaClampUv(uv + vec2( texel.x,  texel.y));
    float lumaNW = fxaaLuma(texture2D(inputColor, uvNW).rgb);
    float lumaNE = fxaaLuma(texture2D(inputColor, uvNE).rgb);
    float lumaSW = fxaaLuma(texture2D(inputColor, uvSW).rgb);
    float lumaSE = fxaaLuma(texture2D(inputColor, uvSE).rgb);

    float edgeHorizontal =
          abs(-2.0 * lumaM + lumaN + lumaS) * 2.0
        + abs(-2.0 * lumaE + lumaNE + lumaSE)
        + abs(-2.0 * lumaW + lumaNW + lumaSW);
    float edgeVertical =
          abs(-2.0 * lumaM + lumaE + lumaW) * 2.0
        + abs(-2.0 * lumaN + lumaNE + lumaNW)
        + abs(-2.0 * lumaS + lumaSE + lumaSW);
    bool isHorizontal = edgeHorizontal >= edgeVertical;

    float lumaNegative = isHorizontal ? lumaN : lumaW;
    float lumaPositive = isHorizontal ? lumaS : lumaE;
    float gradientNegative = abs(lumaNegative - lumaM);
    float gradientPositive = abs(lumaPositive - lumaM);
    bool useNegative = gradientNegative >= gradientPositive;
    float gradient = max(gradientNegative, gradientPositive);
    float lumaLocalAverage = 0.5 *
        (lumaM + (useNegative ? lumaNegative : lumaPositive));

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
    float deltaNegative =
        fxaaLuma(texture2D(inputColor, uvNegative).rgb) - lumaLocalAverage;
    float deltaPositive =
        fxaaLuma(texture2D(inputColor, uvPositive).rgb) - lumaLocalAverage;
    bool doneNegative = abs(deltaNegative) >= gradientThreshold;
    bool donePositive = abs(deltaPositive) >= gradientThreshold;
    float distanceNegative = 1.0;
    float distancePositive = 1.0;

    if (!doneNegative) {
        uvNegative = fxaaClampUv(uvNegative - tangentStep);
        distanceNegative = 2.0;
        deltaNegative =
            fxaaLuma(texture2D(inputColor, uvNegative).rgb) - lumaLocalAverage;
        doneNegative = abs(deltaNegative) >= gradientThreshold;
    }
    if (!donePositive) {
        uvPositive = fxaaClampUv(uvPositive + tangentStep);
        distancePositive = 2.0;
        deltaPositive =
            fxaaLuma(texture2D(inputColor, uvPositive).rgb) - lumaLocalAverage;
        donePositive = abs(deltaPositive) >= gradientThreshold;
    }
    if (!doneNegative) {
        uvNegative = fxaaClampUv(uvNegative - tangentStep * 2.0);
        distanceNegative = 4.0;
        deltaNegative =
            fxaaLuma(texture2D(inputColor, uvNegative).rgb) - lumaLocalAverage;
        doneNegative = abs(deltaNegative) >= gradientThreshold;
    }
    if (!donePositive) {
        uvPositive = fxaaClampUv(uvPositive + tangentStep * 2.0);
        distancePositive = 4.0;
        deltaPositive =
            fxaaLuma(texture2D(inputColor, uvPositive).rgb) - lumaLocalAverage;
        donePositive = abs(deltaPositive) >= gradientThreshold;
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
    bool correctVariation = endpointIsDarker != centerIsDarker;
    float nearestDistance = min(distanceNegative, distancePositive);
    float edgeOffset = 0.5 - nearestDistance /
        max(distanceNegative + distancePositive, 0.0001);
    if (!correctVariation) {
        edgeOffset = 0.0;
    }

    float lumaAverage = (2.0 * (lumaN + lumaE + lumaS + lumaW)
        + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    float subpixel = clamp(abs(lumaAverage - lumaM) /
        max(lumaRange, 0.0001), 0.0, 1.0);
    subpixel = smoothstep(0.0, 1.0, subpixel);
    float subpixelOffset = subpixel * subpixel * fxaaQuality.z;

    float finalOffset = max(edgeOffset, subpixelOffset);
    vec2 finalUv = fxaaClampUv(uv + normalStep * finalOffset);
    vec3 filtered = texture2D(inputColor, finalUv).rgb;
    gl_FragColor = vec4(filtered, colorM.a);
}
)";

constexpr const char* kFxaaCacheKey = "fxaa_quality_edge_search_v2";

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
