#include "detail/BloomExtractPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomPipeline.h"
#include "detail/FgResource.h"          // §F2 (2026-07-24) — FrameGraph FgResourceId::BloomBright
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/SceneColorPipeline.h"

#include "AYRenderer/BloomShaderSources.h"
#include "AYShader/ShaderResource.h"

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// Mirror PostProcessPass — single oversize fullscreen triangle covers
// the entire viewport without a diagonal seam (bgfx 00-helloworld
// pattern). UV.y flip handled in FS for D3D RT vs backbuffer convention.
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

uint32_t BloomExtractPass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;

    // Backend gates precede every FrameGraph resolve and GPU allocation.
    if (!adapter.isInitialized()) {
        return 0;
    }
    if (adapter.isNoopBackend()) {
        // Same rationale as PostProcessPass::execute: Noop backend
        // returns valid handles for everything (so handle-validity
        // can't distinguish "real backend that's broken" from "Noop
        // that should skip"). Skip the pass entirely — preserves
        // the S1a K1 invariant #2 (Noop ⇒ no FBO created + 0 draws).
        return 0;
    }

    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }

    const bgfx::FrameBufferHandle sourceFbo =
        selectSceneColorSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }

    const bgfx::TextureHandle fboColor = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(fboColor)) {
        return 0;
    }

    // BloomBright is a borrowed FrameGraph target. The graph does not declare
    // it when the full Extract+Blur capability is disabled or incomplete.
    if (ctx.frameGraph == nullptr) {
        // Pre-F2 callers (legacy test sites) never wire frameGraph
        // ⇒ early-return 0. Byte-equivalent to today's path only
        // when host bloomStrength == 0; for bloomStrength > 0
        // callers must update to wire FrameGraph (Renderer::render
        // does this — pre-F2 caller means a hand-rolled test).
        return 0;
    }
    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::BloomBright);
    if (!BGFXAdapter::isValid(target)) {
        return 0;
    }

    // Half-resolution size uses the same round-up convention as FrameGraph.
    const uint16_t halfW = static_cast<uint16_t>((viewportWidth  + 1u) / 2u);
    const uint16_t halfH = static_cast<uint16_t>((viewportHeight + 1u) / 2u);

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensureProgram(pool);
    const bool programReady = _program.isValid()
        && _uBloomThreshold != ayt::shader::InvalidBinding
        && _uSourceTexelSize != ayt::shader::InvalidBinding
        && _tSceneColor     != ayt::shader::InvalidBinding;
    if (!programReady) {
        // Keep the production latch false; Blur requires a successful current
        // frame Extract submit and therefore cannot sample stale data.
        return 0;
    }

    // Bind the half-res FBO as the draw target. Do NOT bind it as
    // sampler (mirror PostProcessPass::execute — same-FBO feedback
    // clears / blacks the half-res buffer for the next frame).
    constexpr uint8_t viewId = kBloomExtractViewId;
    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();

    adapter.setViewFrameBuffer(viewId, target);
    adapter.setViewRect(viewId, 0, 0, halfW, halfH);
    adapter.setViewTransform(viewId, identity, identity);
    // Don't clear — we always overwrite every pixel via fullscreen
    // triangle. Clearing wastes a depth-stencil resolve on some
    // backends.
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);

    const ayt::shader::TextureHandle texHandle =
        ayt::render::detail::toShaderTexture(fboColor);

    const float thresholdPad[4] = {
        sanitizeBloomThreshold(ctx.bloomThreshold),
        sanitizeBloomSoftKnee(ctx.bloomSoftKnee),
        0.0f, 0.0f
    };
    const float sourceTexelPad[4] = {
        1.0f / static_cast<float>(viewportWidth),
        1.0f / static_cast<float>(viewportHeight),
        0.0f, 0.0f
    };

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    _program.setTexture(0, _tSceneColor, texHandle);
    _program.setUniform(_uBloomThreshold, thresholdPad, sizeof(thresholdPad));
    _program.setUniform(_uSourceTexelSize,
                        sourceTexelPad, sizeof(sourceTexelPad));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;  // depth-ignore state owned by adapter preset
    adapter.setStateDepthTestAlways();  // mirror PostProcessPass
    _program.submit(sub);
    _producedThisFrame = true;

    // Do NOT setViewFrameBuffer(viewId, INVALID) after submit — in bgfx
    // the last bind wins for the whole view this frame, which would
    // redirect the half-res draw to the default backbuffer at (0,0)
    // (tiny duplicate under Editor chrome). Leave the view bound to
    // the FrameGraph target for the frame (same pattern as FO → sceneFbo).

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        std::fprintf(stderr,
            "[BloomExtractPass] first blit view=%u srcFbo=%u "
            "half=%ux%u threshold=%.2f knee=%.2f "
            "(strength applied in Final PP=%.2f)\n",
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(sourceFbo.idx),
            static_cast<unsigned>(halfW),
            static_cast<unsigned>(halfH),
            thresholdPad[0],
            thresholdPad[1],
            ctx.frame.bloomStrength);
        s_loggedFirst = true;
    }
    return 1;
}

void BloomExtractPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    // Mirror PostProcessPass — funnel VB/IB through BGFXAdapter
    // (cutsheet §7 red line: Pass files never call bgfx:: directly).
    // Layout MUST match FullscreenVertex {x,y,u,v}: a 0-stride
    // bgfx::VertexLayout triggers bgfx::fatal under Debug.
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    (void)ensureFullscreenTriangleBuffers(
        adapter, _fullscreenVB, _fullscreenIB,
        kFullscreenTriangle, sizeof(kFullscreenTriangle), layout,
        kFullscreenIndices, sizeof(kFullscreenIndices));
}

void BloomExtractPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    // A source-contract change bumps the static cache-key pointer and forces
    // a fresh acquire.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != ayt::render::kBloomExtractCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = ayt::render::kBloomExtractCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    ayt::shader::ShaderResource acquired =
        pool.acquire(ayt::render::kBloomExtractPhoskiaSource,
                     ayt::render::kBloomExtractCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[BloomExtractPass] Phoskia acquire failed; "
                     "bloom-extract will skip (S1a = 0 draw).\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[BloomExtractPass]   %s\n", err.c_str());
        }
        return;
    }
    _program        = acquired;
    _uBloomThreshold = _program.getUniformBinding("bloomThreshold");
    _uSourceTexelSize = _program.getUniformBinding("sourceTexelSize");
    _tSceneColor     = _program.getTextureBinding("sceneColor");
}

void BloomExtractPass::destroyResources(BGFXAdapter& adapter)
{
    // §F2 (2026-07-24) — Pass 不再 own `_fbo`(迁出到 FrameGraph);
    // 这里只释放 program / VB / IB。FG own 的 transient RT 由
    // FrameGraph::shutdown() 释放(在 Impl shutdown 路径调)。
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    if (_program.isValid()) {
        // Mirror PostProcessPass::destroyResources — ShaderResource
        // carries no back-pointer to its pool; Renderer::Impl owns
        // the pool and outlives this pass (see AYRenderer.cpp:160
        // shutdown order: resources → shaderPool → adapter).
        // ShaderResource::reset decrements the refcount; the pool
        // dtor releases the underlying GPU program when the
        // refcount hits zero.
        _program.reset();
    }
    _uBloomThreshold = ayt::shader::InvalidBinding;
    _uSourceTexelSize = ayt::shader::InvalidBinding;
    _tSceneColor     = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
    _producedThisFrame = false;
}

} // namespace ayt::render::detail
