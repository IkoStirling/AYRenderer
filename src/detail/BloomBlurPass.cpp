#include "detail/BloomBlurPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomExtractPass.h"
#include "detail/FgResource.h"        // §F3 (2026-07-24) — FrameGraph resolvePingPong
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"
#include "detail/RenderResourceBlackboard.h"

#include "AYRenderer/BloomShaderSources.h"
#include "AYShader/ShaderResource.h"

#include <cstdio>
#include <utility>

namespace ayt::render::detail
{

namespace {

// Mirror S1a BloomExtractPass — single oversize fullscreen triangle
// covers the entire viewport without a diagonal seam (bgfx
// 00-helloworld pattern). UV.y flip handled in FS for D3D RT vs
// backbuffer convention.
struct alignas(16) FullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr FullscreenVertex kFullscreenTriangle[3] = {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    {  3.0f, -1.0f, 2.0f, 1.0f },
    { -1.0f,  3.0f, 0.0f, -1.0f },
};

constexpr uint16_t kFullscreenIndices[3] = { 0, 1, 2 };

} // namespace

uint32_t BloomBlurPass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;

    // Mirror S1a BloomExtractPass + PostProcessPass / ShadowPass /
    // LightingPass / SkyboxPass — Noop + uninit short-circuits must
    // come FIRST so headless tests run clean. The FBO create path
    // inside the FG resolve() would otherwise race against
    // bgfx::createFrameBuffer with no init context.
    if (!adapter.isInitialized()) {
        return 0;
    }
    if (adapter.isNoopBackend()) {
        // Same rationale as S1a / PostProcessPass::execute: Noop
        // backend returns valid handles for everything (so handle-
        // validity can't distinguish "real backend that's broken"
        // from "Noop that should skip"). Skip the pass entirely —
        // preserves the S1b K2 invariant #2 (Noop ⇒ no FBO created
        // + 0 draws).
        return 0;
    }

    // FrameGraph owns every bloom target. Production uses the blackboard's
    // BloomBright entry; direct contexts retain the legacy producer latch.
    if (ctx.frameGraph == nullptr) {
        return 0;
    }
    const BlackboardResourceEntry* bloomBright =
        ctx.resourceBlackboard != nullptr
            ? ctx.resourceBlackboard->findProduced(
                  BlackboardResourceId::BloomBright)
            : nullptr;
    const bool extractReady = ctx.resourceBlackboard != nullptr
        ? bloomBright != nullptr
        : ctx.bloomExtractPass != nullptr
            && ctx.bloomExtractPass->producedThisFrame();
    if (!extractReady) {
        return 0;
    }

    const uint16_t viewportWidth = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }
    const auto scaled = [](uint16_t value, uint16_t divisor) {
        return static_cast<uint16_t>((value + divisor - 1u) / divisor);
    };
    const uint16_t halfW = scaled(viewportWidth, 2);
    const uint16_t halfH = scaled(viewportHeight, 2);
    const uint16_t quarterW = scaled(viewportWidth, 4);
    const uint16_t quarterH = scaled(viewportHeight, 4);
    const uint16_t eighthW = scaled(viewportWidth, 8);
    const uint16_t eighthH = scaled(viewportHeight, 8);
    const uint16_t sixteenthW = scaled(viewportWidth, 16);
    const uint16_t sixteenthH = scaled(viewportHeight, 16);

    struct Target final {
        FgResourceId id;
        bgfx::FrameBufferHandle fbo;
        bgfx::TextureHandle texture;
        uint16_t width;
        uint16_t height;
    };
    const auto resolveTarget = [&](FgResourceId id, uint16_t width,
                                   uint16_t height) -> Target {
        const bgfx::FrameBufferHandle fbo = ctx.frameGraph->resolve(id);
        return {id, fbo,
                BGFXAdapter::isValid(fbo)
                    ? adapter.getFboAttachment(fbo, 0)
                    : bgfx::TextureHandle{BGFX_INVALID_HANDLE},
                width, height};
    };

    const Target quarter = resolveTarget(FgResourceId::BloomPyramidQuarter,
                                         quarterW, quarterH);
    const Target eighth = resolveTarget(FgResourceId::BloomPyramidEighth,
                                        eighthW, eighthH);
    const Target sixteenth = resolveTarget(
        FgResourceId::BloomPyramidSixteenth, sixteenthW, sixteenthH);
    const Target upEighth = resolveTarget(FgResourceId::BloomPyramidUpEighth,
                                          eighthW, eighthH);
    const Target upQuarter = resolveTarget(
        FgResourceId::BloomPyramidUpQuarter, quarterW, quarterH);
    const Target blurA = resolveTarget(FgResourceId::BloomBlurA, halfW, halfH);
    const Target blurB = resolveTarget(FgResourceId::BloomBlurB, halfW, halfH);
    const Target targets[] = {
        quarter, eighth, sixteenth, upEighth, upQuarter, blurA, blurB};
    for (const Target& target : targets) {
        if (!BGFXAdapter::isValid(target.fbo)
            || !BGFXAdapter::isValid(target.texture)) {
            return 0;
        }
    }

    // Read the producer's RT0 attachment. BloomBright lives on FG
    // (F2 migration resolved BloomExtract's write target through
    // FG too); we sample it as a texture but never bind it as our
    // draw target (would clear/black the upstream buffer).
    const bgfx::FrameBufferHandle sourceFbo =
        ctx.resourceBlackboard != nullptr
            ? bloomBright->framebuffer
            : ctx.frameGraph->resolve(FgResourceId::BloomBright);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }
    _sourceRt = ctx.resourceBlackboard != nullptr
        ? bloomBright->texture : adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(_sourceRt)) {
        return 0;
    }

    _pingRt = blurA.texture;

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensurePrograms(pool);
    const bool programReady = _downsampleProgram.isValid()
        && _upsampleProgram.isValid()
        && _uDownsampleTexelSize != ayt::shader::InvalidBinding
        && _tDownsampleSource != ayt::shader::InvalidBinding
        && _uUpsampleTexelSize != ayt::shader::InvalidBinding
        && _uBloomCombine != ayt::shader::InvalidBinding
        && _tLowSource != ayt::shader::InvalidBinding
        && _tHighSource != ayt::shader::InvalidBinding;
    if (!programReady) {
        // Acquire failed (shaderc missing on CI). Skip the draw so
        // BloomBlurA/B RTs stay clear (S1c consumer samples zero
        // and produces no bloom; visually identical to
        // bloomStrength=0 host).
        return 0;
    }

    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();

    const auto bindCommon = [&]() {
        adapter.setTransformIdentity();
        adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
        adapter.setIndexBuffer(_fullscreenIB, 0, 3);
        adapter.setStateDepthTestAlways();
    };
    const auto configureTarget = [&](uint8_t viewId, const Target& target) {
        adapter.setViewFrameBuffer(viewId, target.fbo);
        adapter.setViewRect(viewId, 0, 0, target.width, target.height);
        adapter.setViewTransform(viewId, identity, identity);
        adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    };
    const auto submitDown = [&](uint8_t viewId, bgfx::TextureHandle source,
                                uint16_t sourceW, uint16_t sourceH,
                                const Target& target) {
        configureTarget(viewId, target);
        bindCommon();
        const float texel[4] = {
            1.0f / static_cast<float>(sourceW),
            1.0f / static_cast<float>(sourceH), 0.0f, 0.0f};
        _downsampleProgram.setTexture(
            0, _tDownsampleSource, toShaderTexture(source));
        _downsampleProgram.setUniform(
            _uDownsampleTexelSize, texel, sizeof(texel));
        ayt::shader::DrawCallContext sub{};
        sub.viewId = viewId;
        _downsampleProgram.submit(sub);
    };
    const auto submitUp = [&](uint8_t viewId, bgfx::TextureHandle low,
                              uint16_t lowW, uint16_t lowH,
                              bgfx::TextureHandle high, float lowWeight,
                              float highWeight, const Target& target) {
        configureTarget(viewId, target);
        bindCommon();
        const float texel[4] = {
            1.0f / static_cast<float>(lowW),
            1.0f / static_cast<float>(lowH), 0.0f, 0.0f};
        const float combine[4] = {lowWeight, highWeight, 0.0f, 0.0f};
        _upsampleProgram.setTexture(0, _tLowSource, toShaderTexture(low));
        _upsampleProgram.setTexture(0, _tHighSource, toShaderTexture(high));
        _upsampleProgram.setUniform(
            _uUpsampleTexelSize, texel, sizeof(texel));
        _upsampleProgram.setUniform(_uBloomCombine, combine, sizeof(combine));
        ayt::shader::DrawCallContext sub{};
        sub.viewId = viewId;
        _upsampleProgram.submit(sub);
    };

    submitDown(kBloomDownQuarterViewId, _sourceRt, halfW, halfH, quarter);
    submitDown(kBloomDownEighthViewId, quarter.texture, quarterW, quarterH,
               eighth);
    submitDown(kBloomDownSixteenthViewId, eighth.texture, eighthW, eighthH,
               sixteenth);

    constexpr float kScatter = 0.70f;
    submitUp(kBloomUpEighthViewId, sixteenth.texture, sixteenthW, sixteenthH,
             eighth.texture, kScatter, 1.0f, upEighth);
    submitUp(kBloomUpQuarterViewId, upEighth.texture, eighthW, eighthH,
             quarter.texture, kScatter, 1.0f, upQuarter);
    submitUp(kBloomUpHalfViewId, upQuarter.texture, quarterW, quarterH,
             _sourceRt, kScatter, 1.0f, blurA);
    // Final tent resolve smooths the level transition without adding the
    // bright root a second time.
    submitUp(kBloomResolveViewId, blurA.texture, halfW, halfH,
             blurA.texture, 1.0f, 0.0f, blurB);

    _producedThisFrame = true;
    ctx.frameGraph->markProduced(FgResourceId::BloomPyramidQuarter);
    ctx.frameGraph->markProduced(FgResourceId::BloomPyramidEighth);
    ctx.frameGraph->markProduced(FgResourceId::BloomPyramidSixteenth);
    ctx.frameGraph->markProduced(FgResourceId::BloomPyramidUpEighth);
    ctx.frameGraph->markProduced(FgResourceId::BloomPyramidUpQuarter);
    ctx.frameGraph->markProduced(FgResourceId::BloomBlurA);
    ctx.frameGraph->markProduced(FgResourceId::BloomBlurB);
    if (ctx.resourceBlackboard != nullptr) {
        ctx.resourceBlackboard->publishProduced(
            BlackboardResourceId::BloomBlurA,
            BlackboardResourceLifetime::Transient,
            blurA.fbo, blurA.texture, halfW, halfH, 0);
        ctx.resourceBlackboard->publishProduced(
            BlackboardResourceId::BloomBlurB,
            BlackboardResourceLifetime::Transient,
            blurB.fbo, blurB.texture, halfW, halfH, 0);
    }

    // Do NOT restore views to INVALID after submit — last
    // setViewFrameBuffer wins per view for the frame and would paint
    // half-res blur onto the default backbuffer at (0,0).

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        std::fprintf(stderr,
            "[BloomBlurPass] first pyramid dispatch views=(%u..%u) srcFbo=%u "
            "pingFbo=%u pongFbo=%u half=%ux%u lowest=%ux%u\n",
            static_cast<unsigned>(kBloomDownQuarterViewId),
            static_cast<unsigned>(kBloomResolveViewId),
            static_cast<unsigned>(sourceFbo.idx),
            static_cast<unsigned>(blurA.fbo.idx),
            static_cast<unsigned>(blurB.fbo.idx),
            static_cast<unsigned>(halfW),
            static_cast<unsigned>(halfH),
            static_cast<unsigned>(sixteenthW),
            static_cast<unsigned>(sixteenthH));
        s_loggedFirst = true;
    }
    return 7;
}

void BloomBlurPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    // Mirror S1a BloomExtractPass — funnel VB/IB through BGFXAdapter
    // (cutsheet §7 red line: Pass files never call bgfx:: directly).
    // Layout MUST match FullscreenVertex {x,y,u,v}: a 0-stride
    // bgfx::VertexLayout triggers bgfx::fatal under Debug.
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    (void)ensureFullscreenTriangleBuffers(
        adapter, _fullscreenVB, _fullscreenIB,
        kFullscreenTriangle, sizeof(kFullscreenTriangle), layout,
        kFullscreenIndices, sizeof(kFullscreenIndices));
}

void BloomBlurPass::ensurePrograms(shader::ShaderResourcePool& pool)
{
    static const char* s_downsampleKey = nullptr;
    static const char* s_upsampleKey = nullptr;
    if (s_downsampleKey != ayt::render::kBloomPyramidDownsampleCacheKey
        || s_upsampleKey != ayt::render::kBloomPyramidUpsampleCacheKey) {
        _downsampleProgram.reset();
        _upsampleProgram.reset();
        _programAcquireFailed = false;
        s_downsampleKey = ayt::render::kBloomPyramidDownsampleCacheKey;
        s_upsampleKey = ayt::render::kBloomPyramidUpsampleCacheKey;
    }

    if ((_downsampleProgram.isValid() && _upsampleProgram.isValid())
        || _programAcquireFailed) {
        return;
    }
    ayt::shader::ShaderResource downsample =
        pool.acquire(ayt::render::kBloomPyramidDownsamplePhoskiaSource,
                     ayt::render::kBloomPyramidDownsampleCacheKey);
    ayt::shader::ShaderResource upsample =
        pool.acquire(ayt::render::kBloomPyramidUpsamplePhoskiaSource,
                     ayt::render::kBloomPyramidUpsampleCacheKey);
    if (!downsample.isValid() || !upsample.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[BloomBlurPass] Phoskia acquire failed; "
                     "bloom pyramid will skip.\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[BloomBlurPass]   %s\n", err.c_str());
        }
        return;
    }
    _downsampleProgram = std::move(downsample);
    _upsampleProgram = std::move(upsample);
    _uDownsampleTexelSize =
        _downsampleProgram.getUniformBinding("sourceTexelSize");
    _tDownsampleSource = _downsampleProgram.getTextureBinding("source");
    _uUpsampleTexelSize =
        _upsampleProgram.getUniformBinding("lowTexelSize");
    _uBloomCombine = _upsampleProgram.getUniformBinding("bloomCombine");
    _tLowSource = _upsampleProgram.getTextureBinding("lowSource");
    _tHighSource = _upsampleProgram.getTextureBinding("highSource");
}

void BloomBlurPass::destroyResources(BGFXAdapter& adapter)
{
    // §F3 (2026-07-24) — FBO destroy block removed. The ping/pong
    // RTs live on the FrameGraph now and are released by
    // FrameGraph::shutdown / FrameGraph::resize (Renderer::Impl
    // shutdown path calls fg.shutdown()). This Pass only owns:
    // fullscreen VB/IB + Phoskia program. Mirror BloomExtractPass
    // F2 contract.
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    _downsampleProgram.reset();
    _upsampleProgram.reset();
    _uDownsampleTexelSize = ayt::shader::InvalidBinding;
    _tDownsampleSource = ayt::shader::InvalidBinding;
    _uUpsampleTexelSize = ayt::shader::InvalidBinding;
    _uBloomCombine = ayt::shader::InvalidBinding;
    _tLowSource = ayt::shader::InvalidBinding;
    _tHighSource = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
    _producedThisFrame = false;
    _sourceRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _pingRt   = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
}

} // namespace ayt::render::detail
