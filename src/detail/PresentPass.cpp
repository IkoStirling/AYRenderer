#include "detail/PresentPass.h"

#include "detail/FgResource.h"
#include "detail/GpuResources.h"
#include <cstdio>
#include <string>
#include <utility>

namespace ayt::render::detail
{

namespace {

constexpr const char* kPresentPhoskiaSource = R"(
material Present {
    texture2d finalColor
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        return sample(finalColor, uv)
    }
}
)";

constexpr const char* kPresentCacheKey =
    "present_final_ldr_blit_v1";

} // namespace

const char* const kPresentCacheKeyCStr = kPresentCacheKey;

const char* presentPhoskiaSourceForTests() noexcept
{
    return kPresentPhoskiaSource;
}

uint32_t PresentPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    if (!adapter.isInitialized() || adapter.isNoopBackend()
        || ctx.viewportWidth == 0 || ctx.viewportHeight == 0
        || ctx.frameGraph == nullptr
        || !ctx.frameGraph->semanticProducedThisFrame(
            FgSemantic::PresentSource)) {
        return 0;
    }

    const bgfx::FrameBufferHandle source =
        ctx.frameGraph->resolveSemantic(FgSemantic::PresentSource);
    if (!BGFXAdapter::isValid(source)) {
        rateLimitedEarlyReturn("PresentPass", "PresentSource invalid");
        return 0;
    }
    const bgfx::TextureHandle color = adapter.getFboAttachment(source, 0);
    if (!BGFXAdapter::isValid(color)) {
        rateLimitedEarlyReturn("PresentPass", "PresentSource color invalid");
        return 0;
    }

    if (!_geometry.ensure(adapter)) {
        rateLimitedEarlyReturn("PresentPass", "fullscreen geometry invalid");
        return 0;
    }
    ensureProgram(ctx.pool);
    if (!_program.isValid()
        || _tFinalColor == ayt::shader::InvalidBinding) {
        rateLimitedEarlyReturn("PresentPass", "program unavailable");
        return 0;
    }

    configureFullscreenPassView(adapter,
                                kPresentViewId,
                                BGFX_INVALID_HANDLE,
                                ctx.viewportX,
                                ctx.viewportY,
                                ctx.viewportWidth,
                                ctx.viewportHeight);
    _geometry.bind(adapter);
    _program.setTexture(0, _tFinalColor, toShaderTexture(color));
    adapter.setStateDepthTestAlways();

    ayt::shader::DrawCallContext draw;
    draw.viewId = kPresentViewId;
    draw.state = 0;
    _program.submit(draw);
    return 1;
}

void PresentPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    if (_program.isValid()) {
        return;
    }
    if (_programRetryFrames > 0) {
        --_programRetryFrames;
        return;
    }

    ayt::shader::ShaderResource program =
        pool.acquire(kPresentPhoskiaSource, kPresentCacheKey);
    const ayt::shader::BindingId finalColor = program.isValid()
        ? program.getTextureBinding("finalColor")
        : ayt::shader::InvalidBinding;
    if (!program.isValid() || finalColor == ayt::shader::InvalidBinding) {
        constexpr uint16_t kRetryIntervalFrames = 120;
        _programRetryFrames = kRetryIntervalFrames;
        std::fprintf(stderr,
                     "[PresentPass] shader acquire/binding failed; retrying "
                     "in %u frames.\n",
                     static_cast<unsigned>(kRetryIntervalFrames));
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[PresentPass]   %s\n", error.c_str());
        }
        return;
    }

    _program = std::move(program);
    _tFinalColor = finalColor;
    _programRetryFrames = 0;
}

void PresentPass::destroyResources(BGFXAdapter& adapter)
{
    _geometry.destroy(adapter);
    _program.reset();
    _tFinalColor = ayt::shader::InvalidBinding;
    _programRetryFrames = 0;
}

} // namespace ayt::render::detail
