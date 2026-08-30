#include "detail/SSAOPass.h"

#include "AYRenderer/SSAOShaderSources.h"
#include "AYShader/ShaderResource.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/LightingPass.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/SSAOPipeline.h"

#include <cstdio>
#include <string>

namespace ayt::render::detail
{

namespace
{

struct alignas(16) FullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr FullscreenVertex kFullscreenTriangle[3] = {
    {-1.0f, -1.0f, 0.0f, 1.0f},
    { 3.0f, -1.0f, 2.0f, 1.0f},
    {-1.0f,  3.0f, 0.0f, -1.0f},
};

constexpr uint16_t kFullscreenIndices[3] = {0, 1, 2};

} // namespace

const char* const kSSAOCacheKeyCStr = ayt::render::kSsaoCacheKey;

uint32_t SSAOPass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;

    // The slot is mounted in every deferred pipeline, while the per-frame
    // feature gate may intentionally omit SSAOTexture. Match the complete
    // central graph gate before resolving its output so every inactive chain
    // (disabled/zero parameters, missing or disabled producer/consumer, or a
    // zero viewport) is a quiet zero-allocation path rather than a false
    // allocation-failure diagnostic.
    if (!selectSsaoStage(
            frame.ssaoEnabled,
            frame.ssaoStrength,
            frame.ssaoRadius,
            /*passPresent=*/true,
            /*passEnabled=*/isEnabled(),
            ctx.gbufferPass != nullptr,
            ctx.gbufferPass != nullptr && ctx.gbufferPass->isEnabled(),
            ctx.lightingPass != nullptr,
            ctx.lightingPass != nullptr && ctx.lightingPass->isEnabled(),
            ctx.viewportWidth,
            ctx.viewportHeight)) {
        return 0;
    }

    if (!adapter.isInitialized()) {
        rateLimitedEarlyReturn("SSAOPass", "adapter not initialized");
        return 0;
    }
    if (adapter.isNoopBackend()) {
        rateLimitedEarlyReturn("SSAOPass", "noop backend");
        return 0;
    }
    if (ctx.frameGraph == nullptr) {
        rateLimitedEarlyReturn("SSAOPass", "ctx.frameGraph == nullptr");
        return 0;
    }

    const uint16_t viewportWidth = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;

    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::SSAOTexture);
    if (!BGFXAdapter::isValid(target)) {
        rateLimitedEarlyReturn("SSAOPass", "SSAOTexture resolve invalid");
        return 0;
    }

    if (ctx.gbufferPass == nullptr
        || !ctx.gbufferPass->producedThisFrame()) {
        rateLimitedEarlyReturn("SSAOPass", "gbuffer not produced this frame");
        return 0;
    }

    const bgfx::TextureHandle worldPosRt =
        ctx.gbufferPass->gbufferWorldPositionRt();
    const bgfx::TextureHandle worldNrmRt =
        ctx.gbufferPass->gbufferNormalRt();
    const bgfx::TextureHandle coverageRt =
        ctx.gbufferPass->gbufferMaterialRt();
    if (!BGFXAdapter::isValid(worldPosRt)
        || !BGFXAdapter::isValid(worldNrmRt)
        || !BGFXAdapter::isValid(coverageRt)) {
        rateLimitedEarlyReturn(
            "SSAOPass", "gbuffer world-position/normal/coverage RT invalid");
        return 0;
    }

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        rateLimitedEarlyReturn("SSAOPass", "fullscreen VB/IB invalid");
        return 0;
    }

    ensureProgram(pool);
    const bool programReady = _program.isValid()
        && _uSSAORadius != ayt::shader::InvalidBinding
        && _uSSAOBias != ayt::shader::InvalidBinding
        && _tWorldPosition != ayt::shader::InvalidBinding
        && _tWorldNormal != ayt::shader::InvalidBinding
        && _tGeometryCoverage != ayt::shader::InvalidBinding;
    if (!programReady) {
        rateLimitedEarlyReturn("SSAOPass", "program not ready");
        return 0;
    }

    constexpr uint8_t viewId = kSsaoViewId;
    adapter.setViewFrameBuffer(viewId, target);
    adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, frame.view, frame.projection);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_COLOR, 0x00000000u);

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);

    const ayt::shader::TextureHandle worldPosHandle =
        ayt::render::detail::toShaderTexture(worldPosRt);
    const ayt::shader::TextureHandle worldNrmHandle =
        ayt::render::detail::toShaderTexture(worldNrmRt);
    const ayt::shader::TextureHandle coverageHandle =
        ayt::render::detail::toShaderTexture(coverageRt);
    _program.setTexture(_program.getTextureStage(_tWorldPosition),
                        _tWorldPosition,
                        worldPosHandle);
    _program.setTexture(_program.getTextureStage(_tWorldNormal),
                        _tWorldNormal,
                        worldNrmHandle);
    _program.setTexture(_program.getTextureStage(_tGeometryCoverage),
                        _tGeometryCoverage,
                        coverageHandle);

    const float radiusPad[4] = {frame.ssaoRadius, 0.0f, 0.0f, 0.0f};
    const float biasPad[4] = {frame.ssaoBias, 0.0f, 0.0f, 0.0f};
    _program.setUniform(_uSSAORadius, radiusPad, sizeof(radiusPad));
    _program.setUniform(_uSSAOBias, biasPad, sizeof(biasPad));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state = 0;
    adapter.setStateDepthTestAlways();
    _program.submit(sub);
    _producedThisFrame = true;

    static bool s_loggedFirstDispatch = false;
    if (!s_loggedFirstDispatch) {
        std::fprintf(stderr,
                     "[SSAOPass] first dispatch view=%u viewport=%ux%u "
                     "strength=%.2f radius=%.2f bias=%.3f\n",
                     static_cast<unsigned>(viewId),
                     static_cast<unsigned>(viewportWidth),
                     static_cast<unsigned>(viewportHeight),
                     frame.ssaoStrength,
                     frame.ssaoRadius,
                     frame.ssaoBias);
        s_loggedFirstDispatch = true;
    }
    return 1;
}

void SSAOPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    (void)ensureFullscreenTriangleBuffers(
        adapter, _fullscreenVB, _fullscreenIB,
        kFullscreenTriangle, sizeof(kFullscreenTriangle), layout,
        kFullscreenIndices, sizeof(kFullscreenIndices));
}

void SSAOPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != ayt::render::kSsaoCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = ayt::render::kSsaoCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }

    ayt::shader::ShaderResource acquired =
        pool.acquire(ayt::render::kSsaoPhoskiaSource,
                     ayt::render::kSsaoCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[SSAOPass] Phoskia acquire failed; SSAO will skip.\n");
        for (const std::string& error : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[SSAOPass]   %s\n", error.c_str());
        }
        return;
    }

    _program = acquired;
    _uSSAORadius = _program.getUniformBinding("ssaoRadius");
    _uSSAOBias = _program.getUniformBinding("ssaoBias");
    _tWorldPosition = _program.getTextureBinding("worldPosition");
    _tWorldNormal = _program.getTextureBinding("worldNormal");
    _tGeometryCoverage = _program.getTextureBinding("geometryCoverage");
}

void SSAOPass::destroyResources(BGFXAdapter& adapter)
{
    _producedThisFrame = false;
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    _program.reset();
    _uSSAORadius = ayt::shader::InvalidBinding;
    _uSSAOBias = ayt::shader::InvalidBinding;
    _tWorldPosition = ayt::shader::InvalidBinding;
    _tWorldNormal = ayt::shader::InvalidBinding;
    _tGeometryCoverage = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
}

} // namespace ayt::render::detail
