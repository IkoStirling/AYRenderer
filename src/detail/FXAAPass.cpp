#include "detail/FXAAPass.h"

#include "detail/FgResource.h"
#include "detail/GpuResources.h"

#include <cstdio>
#include <string>
#include <utility>

namespace ayt::render::detail
{

namespace {

// FXAA 3.11-style luma edge search over display-referred FinalLdrColor.
// Every offset is explicitly clamped because FrameGraph targets do not promise
// a particular sampler address mode. The final branch is expressed as mix +
// step so the shader remains portable across the current Phoskia backends.
constexpr const char* kFxaaPhoskiaSource = R"(
material FXAA {
    texture2d inputColor
    uniform vec4 inverseViewport
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let texel = inverseViewport.xy
        let uvNW = clamp(uv + vec2(-texel.x, -texel.y), vec2(0.0, 0.0), vec2(1.0, 1.0))
        let uvNE = clamp(uv + vec2( texel.x, -texel.y), vec2(0.0, 0.0), vec2(1.0, 1.0))
        let uvSW = clamp(uv + vec2(-texel.x,  texel.y), vec2(0.0, 0.0), vec2(1.0, 1.0))
        let uvSE = clamp(uv + vec2( texel.x,  texel.y), vec2(0.0, 0.0), vec2(1.0, 1.0))
        let colorM = sample(inputColor, uv)
        let rgbNW = sample(inputColor, uvNW).xyz
        let rgbNE = sample(inputColor, uvNE).xyz
        let rgbSW = sample(inputColor, uvSW).xyz
        let rgbSE = sample(inputColor, uvSE).xyz
        let luma = vec3(0.299, 0.587, 0.114)
        let lumaNW = dot(rgbNW, luma)
        let lumaNE = dot(rgbNE, luma)
        let lumaSW = dot(rgbSW, luma)
        let lumaSE = dot(rgbSE, luma)
        let lumaM = dot(colorM.xyz, luma)
        let lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)))
        let lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)))
        let dir = vec2(-(lumaNW + lumaNE - lumaSW - lumaSE),
                         lumaNW + lumaSW - lumaNE - lumaSE)
        let dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125)
        let rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce)
        let dirUv = clamp(dir * rcpDirMin, vec2(-8.0, -8.0), vec2(8.0, 8.0)) * texel
        let uvA0 = clamp(uv + dirUv * -0.1666667, vec2(0.0, 0.0), vec2(1.0, 1.0))
        let uvA1 = clamp(uv + dirUv *  0.1666667, vec2(0.0, 0.0), vec2(1.0, 1.0))
        let rgbA = (sample(inputColor, uvA0).xyz + sample(inputColor, uvA1).xyz) * 0.5
        let uvB0 = clamp(uv + dirUv * -0.5, vec2(0.0, 0.0), vec2(1.0, 1.0))
        let uvB1 = clamp(uv + dirUv *  0.5, vec2(0.0, 0.0), vec2(1.0, 1.0))
        let rgbB = rgbA * 0.5 + (sample(inputColor, uvB0).xyz + sample(inputColor, uvB1).xyz) * 0.25
        let lumaB = dot(rgbB, luma)
        let useB = step(lumaMin, lumaB) * (1.0 - step(lumaMax, lumaB))
        return vec4(mix(rgbA, rgbB, useB), colorM.w)
    }
}
)";

constexpr const char* kFxaaCacheKey = "fxaa_311_luma_v1";

} // namespace

const char* const kFxaaCacheKeyCStr = kFxaaCacheKey;

const char* fxaaPhoskiaSourceForTests() noexcept
{
    return kFxaaPhoskiaSource;
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

    ayt::shader::ShaderResource program =
        pool.acquire(kFxaaPhoskiaSource, kFxaaCacheKey);
    const ayt::shader::BindingId inputColor = program.isValid()
        ? program.getTextureBinding("inputColor")
        : ayt::shader::InvalidBinding;
    const ayt::shader::BindingId inverseViewport = program.isValid()
        ? program.getUniformBinding("inverseViewport")
        : ayt::shader::InvalidBinding;
    if (!program.isValid()
        || inputColor == ayt::shader::InvalidBinding
        || inverseViewport == ayt::shader::InvalidBinding) {
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
    _programRetryFrames = 0;
}

void FXAAPass::destroyResources(BGFXAdapter& adapter)
{
    _geometry.destroy(adapter);
    _program.reset();
    _tInputColor = ayt::shader::InvalidBinding;
    _uInverseViewport = ayt::shader::InvalidBinding;
    _programRetryFrames = 0;
}

} // namespace ayt::render::detail
