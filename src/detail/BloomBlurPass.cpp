#include "detail/BloomBlurPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomExtractPass.h"
#include "detail/FgResource.h"        // §F3 (2026-07-24) — FrameGraph resolvePingPong
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"

#include "AYRenderer/BloomShaderSources.h"
#include "AYShader/ShaderResource.h"

#include <cstdio>

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

    // FrameGraph owns every bloom target; the producer pointer supplies only
    // the current-frame completion latch.
    if (ctx.frameGraph == nullptr) {
        return 0;
    }
    if (ctx.bloomExtractPass == nullptr
        || !ctx.bloomExtractPass->producedThisFrame()) {
        return 0;
    }

    // Match FrameGraph's half-resolution round-up convention.
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }
    const uint16_t halfW = static_cast<uint16_t>((viewportWidth  + 1u) / 2u);
    const uint16_t halfH = static_cast<uint16_t>((viewportHeight + 1u) / 2u);

    // A and B are separate physical targets because the vertical pass reads A
    // while writing B. Invalid resolution fails closed before any submit.
    const FgPingPong pp = ctx.frameGraph->resolvePingPong(
        FgResourceId::BloomBlurA, FgResourceId::BloomBlurB);
    if (!BGFXAdapter::isValid(pp.first)
        || !BGFXAdapter::isValid(pp.second)) {
        return 0;
    }

    // Read the producer's RT0 attachment. BloomBright lives on FG
    // (F2 migration resolved BloomExtract's write target through
    // FG too); we sample it as a texture but never bind it as our
    // draw target (would clear/black the upstream buffer).
    const bgfx::FrameBufferHandle sourceFbo =
        ctx.frameGraph->resolve(FgResourceId::BloomBright);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }
    _sourceRt = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(_sourceRt)) {
        return 0;
    }

    // Refresh attachment handles lazily (mirror LightingPass
    // ::cacheAttachments pattern). Cheap; cache only invalidates
    // when FG recreates the RT after a size change.
    _pingRt = adapter.getFboAttachment(pp.first, 0);

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensureProgram(pool);
    const bool programReady = _program.isValid()
        && _uDirection != ayt::shader::InvalidBinding
        && _uTexelSize != ayt::shader::InvalidBinding
        && _tSource    != ayt::shader::InvalidBinding;
    if (!programReady) {
        // Acquire failed (shaderc missing on CI). Skip the draw so
        // BloomBlurA/B RTs stay clear (S1c consumer samples zero
        // and produces no bloom; visually identical to
        // bloomStrength=0 host).
        return 0;
    }

    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();

    // Pixel size — horizontal step = (1/halfW, 0); vertical step =
    // (0, 1/halfH). Stored as vec4 .xy with .zw zero (cutsheet
    // §3.1 vec4 gate). The FS uses direction * texelSize as the
    // offset, so swapping (1,0)/(0,1) pairs selects horizontal vs
    // vertical pass.
    const float texelStepX = 1.0f / static_cast<float>(halfW);
    const float texelStepY = 1.0f / static_cast<float>(halfH);
    const float dirH[4]    = { 1.0f, 0.0f, 0.0f, 0.0f };
    const float texelH[4]  = { texelStepX, 0.0f, 0.0f, 0.0f };
    const float dirV[4]    = { 0.0f, 1.0f, 0.0f, 0.0f };
    const float texelV[4]  = { 0.0f, texelStepY, 0.0f, 0.0f };

    ayt::shader::TextureHandle sourceShaderHandle =
        ayt::render::detail::toShaderTexture(_sourceRt);
    ayt::shader::TextureHandle pingShaderHandle =
        ayt::render::detail::toShaderTexture(_pingRt);

    // bgfx clears draw state after every submit — VB/IB/state must be
    // rebound before EACH pass (H then V). Previously only the first
    // submit had geometry → pong stayed black → Final bloom looked dead.

    // === Pass A: horizontal blur (view 11) ===
    // Source = BloomBright's RT0; target = BloomBlurA (FG ping).
    adapter.setViewFrameBuffer(kBloomBlurHorizontalViewId, pp.first);
    adapter.setViewRect(kBloomBlurHorizontalViewId, 0, 0, halfW, halfH);
    adapter.setViewTransform(kBloomBlurHorizontalViewId, identity, identity);
    adapter.setViewClearRaw(kBloomBlurHorizontalViewId,
                            BGFX_CLEAR_NONE, 0, 1.0f, 0);

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    adapter.setStateDepthTestAlways();
    _program.setTexture(0, _tSource, sourceShaderHandle);
    _program.setUniform(_uDirection, dirH, sizeof(dirH));
    _program.setUniform(_uTexelSize, texelH, sizeof(texelH));

    ayt::shader::DrawCallContext subH;
    subH.viewId = kBloomBlurHorizontalViewId;
    subH.state  = 0;
    _program.submit(subH);

    // === Pass B: vertical blur (view 12) ===
    // Source = BloomBlurA's RT0; target = BloomBlurB (FG pong).
    adapter.setViewFrameBuffer(kBloomBlurVerticalViewId, pp.second);
    adapter.setViewRect(kBloomBlurVerticalViewId, 0, 0, halfW, halfH);
    adapter.setViewTransform(kBloomBlurVerticalViewId, identity, identity);
    adapter.setViewClearRaw(kBloomBlurVerticalViewId,
                            BGFX_CLEAR_NONE, 0, 1.0f, 0);

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    adapter.setStateDepthTestAlways();
    _program.setTexture(0, _tSource, pingShaderHandle);
    _program.setUniform(_uDirection, dirV, sizeof(dirV));
    _program.setUniform(_uTexelSize, texelV, sizeof(texelV));

    ayt::shader::DrawCallContext subV;
    subV.viewId = kBloomBlurVerticalViewId;
    subV.state  = 0;
    _program.submit(subV);
    _producedThisFrame = true;

    // Do NOT restore views to INVALID after submit — last
    // setViewFrameBuffer wins per view for the frame and would paint
    // half-res blur onto the default backbuffer at (0,0).

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        std::fprintf(stderr,
            "[BloomBlurPass] first dispatch views=(%u,%u) srcFbo=%u "
            "pingFbo=%u pongFbo=%u half=%ux%u\n",
            static_cast<unsigned>(kBloomBlurHorizontalViewId),
            static_cast<unsigned>(kBloomBlurVerticalViewId),
            static_cast<unsigned>(sourceFbo.idx),
            static_cast<unsigned>(pp.first.idx),
            static_cast<unsigned>(pp.second.idx),
            static_cast<unsigned>(halfW),
            static_cast<unsigned>(halfH));
        s_loggedFirst = true;
    }
    return 2;  // 2 submits: 1 horizontal + 1 vertical.
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

void BloomBlurPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    // A source-contract change bumps the static cache-key pointer and forces
    // a fresh acquire.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != ayt::render::kBloomBlurCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = ayt::render::kBloomBlurCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    ayt::shader::ShaderResource acquired =
        pool.acquire(ayt::render::kBloomBlurPhoskiaSource,
                     ayt::render::kBloomBlurCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[BloomBlurPass] Phoskia acquire failed; "
                     "bloom-blur will skip (S1b = 0 draw).\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[BloomBlurPass]   %s\n", err.c_str());
        }
        return;
    }
    _program     = acquired;
    _uDirection  = _program.getUniformBinding("direction");
    _uTexelSize  = _program.getUniformBinding("texelSize");
    _tSource     = _program.getTextureBinding("source");
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
    if (_program.isValid()) {
        // Mirror S1a BloomExtractPass::destroyResources —
        // ShaderResource carries no back-pointer to its pool;
        // Renderer::Impl owns the pool and outlives this pass.
        _program.reset();
    }
    _uDirection = ayt::shader::InvalidBinding;
    _uTexelSize = ayt::shader::InvalidBinding;
    _tSource    = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
    _producedThisFrame = false;
    _sourceRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _pingRt   = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
}

} // namespace ayt::render::detail
