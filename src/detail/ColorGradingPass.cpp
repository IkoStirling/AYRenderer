#include "detail/ColorGradingPass.h"

#include "detail/FgResource.h"
#include "detail/GpuResources.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace ayt::render::detail
{

namespace {

constexpr const char* kColorGradingPhoskiaSource = R"(
material ColorGrading {
    texture2d inputColor
    texture2d colorLut
    uniform vec4 gradingParams
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let source = sample(inputColor, uv)
        let size = gradingParams.y
        let maxIndex = size - 1.0
        // Keep these values scalar. Phoskia's current HLSL emitter can infer
        // clamp(vec3, vec3, vec3) as a scalar, which makes scaled.z invalid on
        // the D3D11/D3D12 s_5_0 path even though GLSL accepts the expression.
        let scaledX = clamp(source.x, 0.0, 1.0) * maxIndex
        let scaledY = clamp(source.y, 0.0, 1.0) * maxIndex
        let scaledZ = clamp(source.z, 0.0, 1.0) * maxIndex
        let blue0 = floor(scaledZ)
        let blue1 = min(blue0 + 1.0, maxIndex)
        let blueMix = scaledZ - blue0
        let lutWidth = size * size
        let lutUv0 = vec2((blue0 * size + scaledX + 0.5) / lutWidth,
                          (scaledY + 0.5) / size)
        let lutUv1 = vec2((blue1 * size + scaledX + 0.5) / lutWidth,
                          (scaledY + 0.5) / size)
        let graded0 = sample(colorLut, lutUv0).xyz
        let graded1 = sample(colorLut, lutUv1).xyz
        let graded = mix(graded0, graded1, blueMix)
        return vec4(mix(source.xyz, graded, gradingParams.x), source.w)
    }
}
)";

constexpr const char* kColorGradingCacheKey = "color_grading_lut2d_32_v2";

float saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float contrast(float value, float amount)
{
    return saturate((value - 0.5f) * amount + 0.5f);
}

void applyPreset(ColorGradingPreset preset, float& r, float& g, float& b)
{
    switch (preset) {
    case ColorGradingPreset::Warm:
        r = contrast(std::pow(saturate(r), 0.96f) * 1.035f + 0.008f, 1.03f);
        g = contrast(g * 1.005f + 0.004f, 1.02f);
        b = contrast(std::pow(saturate(b), 1.035f) * 0.945f, 1.03f);
        break;
    case ColorGradingPreset::Cool:
        r = contrast(std::pow(saturate(r), 1.025f) * 0.955f, 1.025f);
        g = contrast(g * 1.005f + 0.003f, 1.02f);
        b = contrast(std::pow(saturate(b), 0.965f) * 1.04f + 0.006f, 1.025f);
        break;
    case ColorGradingPreset::Cinematic: {
        r = contrast(r, 1.10f);
        g = contrast(g, 1.08f);
        b = contrast(b, 1.10f);
        const float luminance = r * 0.299f + g * 0.587f + b * 0.114f;
        const float shadow = 1.0f - saturate(luminance * 2.0f);
        const float highlight = saturate((luminance - 0.5f) * 2.0f);
        r = saturate(r + highlight * 0.045f - shadow * 0.012f);
        g = saturate(g + shadow * 0.018f + highlight * 0.010f);
        b = saturate(b + shadow * 0.050f - highlight * 0.025f);
        break;
    }
    case ColorGradingPreset::Neutral:
    default:
        break;
    }
}

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::lround(saturate(value) * 255.0f));
}

} // namespace

const char* const kColorGradingCacheKeyCStr = kColorGradingCacheKey;

const char* colorGradingPhoskiaSourceForTests() noexcept
{
    return kColorGradingPhoskiaSource;
}

std::vector<uint8_t> generateColorGradingLutRgba8(ColorGradingPreset preset)
{
    constexpr uint32_t size = ColorGradingPass::kLutSize;
    constexpr uint32_t width = size * size;
    std::vector<uint8_t> pixels(width * size * 4u);
    for (uint32_t blue = 0; blue < size; ++blue) {
        for (uint32_t green = 0; green < size; ++green) {
            for (uint32_t red = 0; red < size; ++red) {
                float r = static_cast<float>(red) / static_cast<float>(size - 1u);
                float g = static_cast<float>(green) / static_cast<float>(size - 1u);
                float b = static_cast<float>(blue) / static_cast<float>(size - 1u);
                applyPreset(preset, r, g, b);
                const uint32_t x = blue * size + red;
                const uint32_t index = (green * width + x) * 4u;
                pixels[index + 0u] = toByte(r);
                pixels[index + 1u] = toByte(g);
                pixels[index + 2u] = toByte(b);
                pixels[index + 3u] = 255u;
            }
        }
    }
    return pixels;
}

void ColorGradingPass::setStrength(float strength) noexcept
{
    _strength = std::isfinite(strength)
        ? std::clamp(strength, 0.0f, 1.0f)
        : 0.0f;
}

void ColorGradingPass::setPreset(ColorGradingPreset preset) noexcept
{
    if (static_cast<uint8_t>(preset)
        > static_cast<uint8_t>(ColorGradingPreset::Cinematic)) {
        preset = ColorGradingPreset::Neutral;
    }
    if (_preset != preset) {
        _preset = preset;
        _lutDirty = true;
    }
}

bool ColorGradingPass::isReady() const noexcept
{
    return _geometry.isReady() && _program.isValid()
        && bgfx::isValid(_lutTexture)
        && _tInputColor != ayt::shader::InvalidBinding
        && _tColorLut != ayt::shader::InvalidBinding
        && _uGradingParams != ayt::shader::InvalidBinding;
}

uint32_t ColorGradingPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    if (!adapter.isInitialized() || adapter.isNoopBackend()
        || ctx.viewportWidth == 0 || ctx.viewportHeight == 0
        || ctx.frameGraph == nullptr || _strength <= 0.0f
        || _preset == ColorGradingPreset::Neutral
        || !ctx.frameGraph->semanticProducedThisFrame(FgSemantic::PresentSource)) {
        return 0;
    }

    const bgfx::FrameBufferHandle source =
        ctx.frameGraph->resolveSemantic(FgSemantic::PresentSource);
    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::ColorGradedColor);
    if (!BGFXAdapter::isValid(source)) {
        rateLimitedEarlyReturn("ColorGradingPass", "PresentSource resolve invalid");
        return 0;
    }
    if (!BGFXAdapter::isValid(target)) {
        rateLimitedEarlyReturn("ColorGradingPass", "ColorGradedColor resolve invalid");
        return 0;
    }
    const bgfx::TextureHandle color = adapter.getFboAttachment(source, 0);
    if (!BGFXAdapter::isValid(color)) {
        rateLimitedEarlyReturn("ColorGradingPass", "PresentSource attachment invalid");
        return 0;
    }

    if (!_geometry.ensure(adapter) || !ensureLut(adapter)) {
        rateLimitedEarlyReturn("ColorGradingPass", "geometry/LUT unavailable");
        return 0;
    }
    ensureProgram(ctx.pool);
    if (!isReady()) {
        rateLimitedEarlyReturn("ColorGradingPass", "program unavailable");
        return 0;
    }

    configureFullscreenPassView(adapter,
                                kColorGradingViewId,
                                target,
                                0,
                                0,
                                ctx.viewportWidth,
                                ctx.viewportHeight);
    _geometry.bind(adapter);
    _program.setTexture(_program.getTextureStage(_tInputColor),
                        _tInputColor,
                        toShaderTexture(color));
    _program.setTexture(_program.getTextureStage(_tColorLut),
                        _tColorLut,
                        toShaderTexture(_lutTexture));
    const float gradingParams[4] = {
        _strength,
        static_cast<float>(kLutSize),
        0.0f,
        0.0f,
    };
    _program.setUniform(_uGradingParams,
                        gradingParams,
                        sizeof(gradingParams));
    adapter.setStateDepthTestAlways();

    ayt::shader::DrawCallContext draw;
    draw.viewId = kColorGradingViewId;
    draw.state = 0;
    _program.submit(draw);

    if (!_firstDispatchLogged) {
        std::fprintf(stderr,
                     "[ColorGradingPass] first dispatch view=%u size=%ux%u "
                     "preset=%u strength=%.2f\n",
                     static_cast<unsigned>(kColorGradingViewId),
                     static_cast<unsigned>(ctx.viewportWidth),
                     static_cast<unsigned>(ctx.viewportHeight),
                     static_cast<unsigned>(_preset),
                     static_cast<double>(_strength));
        _firstDispatchLogged = true;
    }

    ctx.frameGraph->markProduced(FgResourceId::ColorGradedColor);
    ctx.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,
                                        FgResourceId::ColorGradedColor);
    return 1;
}

bool ColorGradingPass::ensureLut(BGFXAdapter& adapter)
{
    if (!_lutDirty && bgfx::isValid(_lutTexture)) {
        return true;
    }
    if (bgfx::isValid(_lutTexture)) {
        adapter.destroy(_lutTexture);
        _lutTexture = BGFX_INVALID_HANDLE;
    }
    const std::vector<uint8_t> pixels = generateColorGradingLutRgba8(_preset);
    _lutTexture = adapter.createTexture2D(
        static_cast<uint16_t>(kLutSize * kLutSize),
        kLutSize,
        pixels.data(),
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    _lutDirty = !bgfx::isValid(_lutTexture);
    return !_lutDirty;
}

void ColorGradingPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    if (_program.isValid()) {
        return;
    }
    if (_programRetryFrames > 0) {
        --_programRetryFrames;
        return;
    }

    ayt::shader::ShaderResource program =
        pool.acquire(kColorGradingPhoskiaSource, kColorGradingCacheKey);
    const ayt::shader::BindingId inputColor = program.isValid()
        ? program.getTextureBinding("inputColor")
        : ayt::shader::InvalidBinding;
    const ayt::shader::BindingId colorLut = program.isValid()
        ? program.getTextureBinding("colorLut")
        : ayt::shader::InvalidBinding;
    const ayt::shader::BindingId gradingParams = program.isValid()
        ? program.getUniformBinding("gradingParams")
        : ayt::shader::InvalidBinding;
    if (!program.isValid()
        || inputColor == ayt::shader::InvalidBinding
        || colorLut == ayt::shader::InvalidBinding
        || gradingParams == ayt::shader::InvalidBinding) {
        constexpr uint16_t kRetryIntervalFrames = 120;
        _programRetryFrames = kRetryIntervalFrames;
        std::fprintf(stderr,
                     "[ColorGradingPass] shader acquire/binding failed; "
                     "retrying in %u frames.\n",
                     static_cast<unsigned>(kRetryIntervalFrames));
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[ColorGradingPass]   %s\n", error.c_str());
        }
        return;
    }

    _program = std::move(program);
    _tInputColor = inputColor;
    _tColorLut = colorLut;
    _uGradingParams = gradingParams;
    _programRetryFrames = 0;
}

void ColorGradingPass::destroyResources(BGFXAdapter& adapter)
{
    _geometry.destroy(adapter);
    if (bgfx::isValid(_lutTexture)) {
        adapter.destroy(_lutTexture);
    }
    _lutTexture = BGFX_INVALID_HANDLE;
    _lutDirty = true;
    _firstDispatchLogged = false;
    _program.reset();
    _tInputColor = ayt::shader::InvalidBinding;
    _tColorLut = ayt::shader::InvalidBinding;
    _uGradingParams = ayt::shader::InvalidBinding;
    _programRetryFrames = 0;
}

} // namespace ayt::render::detail
