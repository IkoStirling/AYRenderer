#include "detail/BloomExtractPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"          // §F2 (2026-07-24) — FrameGraph FgResourceId::BloomBright
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"

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

// Bright-extract Phoskia source. Samples LIT scene color from the
// upstream FBO (LightingOutput on Deferred path, sceneFbo on Forward
// path — both via PostProcessPass::selectSourceFbo), subtracts a
// threshold (Karis-style soft knee = max(0, lum - threshold) /
// max(lum, 0.0001) to avoid hard cutoff), multiplies by
// bloomStrength (0 ⇒ pass-through zero contribution —
// zero-behavior-change to existing renders), and writes to the
// half-resolution FBO (BGRA8 — sampler is bilinear by default).
//
// Uniform gates (cutsheet lessons §3.1): all scalars as vec4 + .x
// to satisfy bgfx Vec4 upload ABI. UV.y flipped for D3D RT vs
// backbuffer convention (mirror PostProcessPass FS).
//
// Branchless: converter drops if/for. Threshold/knee formula uses
// `step` + `mix` (mirror Skybox0 / LightingPass pattern).
constexpr const char* kBloomExtractPhoskiaSource = R"(
material BloomExtract {
    texture2d sceneColor
    uniform vec4 bloomThreshold
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let sampled = sample(sceneColor, uv)
        // Rec.709 luminance. LightingOutput is RGBA8 LDR — keep the
        // threshold modest so Editor lit surfaces actually contribute.
        let lum = dot(sampled.xyz, vec3(0.2126, 0.7152, 0.0722))
        let knee  = bloomThreshold.x * 0.5
        let soft  = smoothstep(bloomThreshold.x - knee, bloomThreshold.x + knee, lum)
        // Strength is applied ONLY in Final PP (live slider). Extract
        // always writes the bright plate so bloomStrength=0 ⇒ PP adds 0.
        let outRgb = sampled.xyz * soft
        return vec4(outRgb, sampled.w)
    }
}
)";

// Cache-key bump: remove extract-side strength + lower default threshold.
constexpr const char* kBloomExtractCacheKey = "bloomextract_v1_threshold_ldr_no_strength_fs";

} // namespace

uint32_t BloomExtractPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;

    // Mirror PostProcessPass / ShadowPass / LightingPass / SkyboxPass
    // — Noop + uninit short-circuits must come FIRST so headless
    // tests run clean. The FBO create path inside ensureFbo would
    // otherwise race against bgfx::createFrameBuffer with no init
    // context.
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

    // Source-FBO priority = identical to PostProcessPass (cutsheet
    // §P5 B6 lock — deferred LightingOutput wins over forward
    // sceneFbo; both invalid ⇒ no work). Reuses the static helper
    // so the priority decision stays a single source of truth.
    const bgfx::FrameBufferHandle sourceFbo = PostProcessPass::selectSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }

    const bgfx::TextureHandle fboColor = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(fboColor)) {
        return 0;
    }

    // §F2 (2026-07-24) — BloomBright target RT now owned by
    // FrameGraph instead of this Pass. resolve() returns a
    // borrowed physical handle when the resource is live AND
    // the FrameGraph has created it; otherwise returns invalid.
    //
    // F2 ship-path: host bloomStrength == 0 ⇒ FG compile marks
    // BloomExtract pass disabled ⇒ BloomBright not live ⇒
    // resolve() returns invalid ⇒ this pass returns 0 draws.
    // Byte-equivalent to today's "extract writes zeros into the
    // half-res FBO + Final PP folds bloomStrength=0 into no-op"
    // path. F6 will make FrameGraph actually create the physical
    // RT so host bloomStrength > 0 lights the bloom chain back up.
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

    // Half-resolution size — (W+1)/2 rounds UP so we never sample
    // outside [0,W) on the source texture. Mirror conventional
    // half-res chain math (S1 cutsheet §S1 "ensure(w/2, h/2)").
    // F2 NOTE: F6 will read the actual physical size from FG; for
    // now we still compute half-res locally because FrameGraph
    // physical creation is deferred — and the resulting target
    // is currently invalid, so this draw is skipped anyway.
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
        && _tSceneColor     != ayt::shader::InvalidBinding;
    if (!programReady) {
        // Acquire failed (shaderc missing on CI). Skip the draw so
        // the half-res FBO stays clear (any consumer — S1b blur —
        // would then sample zero and produce no bloom; visually
        // identical to default bloomStrength=0 host with no BloomExtract).
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

    // LDR LightingOutput (RGBA8): 0.85 was too high for Editor lit
    // surfaces — extract stayed black and the strength slider looked
    // dead. Soft-knee around ~0.35 catches highlights without blooming
    // the whole frame. Final PP applies frame.bloomStrength.
    const float thresholdPad[4] = {0.35f, 0.0f, 0.0f, 0.0f};

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    _program.setTexture(0, _tSceneColor, texHandle);
    _program.setUniform(_uBloomThreshold, thresholdPad, sizeof(thresholdPad));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;  // depth-ignore state owned by adapter preset
    adapter.setStateDepthTestAlways();  // mirror PostProcessPass
    _program.submit(sub);

    // Do NOT setViewFrameBuffer(viewId, INVALID) after submit — in bgfx
    // the last bind wins for the whole view this frame, which would
    // redirect the half-res draw to the default backbuffer at (0,0)
    // (tiny duplicate under Editor chrome). Leave the view bound to
    // `_fbo` for the frame (same pattern as FO → sceneFbo).

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        std::fprintf(stderr,
            "[BloomExtractPass] first blit view=%u srcFbo=%u "
            "half=%ux%u threshold=%.2f (strength applied in Final PP=%.2f)\n",
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(sourceFbo.idx),
            static_cast<unsigned>(halfW),
            static_cast<unsigned>(halfH),
            thresholdPad[0],
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
    _fullscreenVB = adapter.createVertexBuffer(kFullscreenTriangle,
                                                sizeof(kFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kFullscreenIndices,
                                              sizeof(kFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void BloomExtractPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    // Cache-key bump forces re-acquire (pointer-equal compare).
    // S1a ships one cache-key; S1b/S1c cuts will bump it.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kBloomExtractCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = kBloomExtractCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    ayt::shader::ShaderResource acquired =
        pool.acquire(kBloomExtractPhoskiaSource, kBloomExtractCacheKey);
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
    _tSceneColor     = _program.getTextureBinding("sceneColor");
    _uBloomStrength  = ayt::shader::InvalidBinding; // strength lives in Final PP only
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
    _uBloomStrength  = ayt::shader::InvalidBinding;
    _tSceneColor     = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
}

} // namespace ayt::render::detail