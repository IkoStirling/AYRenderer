#include "detail/BloomBlurPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/BloomExtractPass.h"
#include "detail/FgResource.h"        // §F3 (2026-07-24) — FrameGraph resolvePingPong
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

// §S1b (2026-07-23) — Separable Gaussian blur Phoskia source.
// Single program + uniform `direction` selects horizontal (1,0) or
// vertical (0,1). 5-tap kernel (Karplus-Strong style, sigma ~1.5):
// weights = [0.227, 0.194, 0.121, 0.054, 0.016] — hand-tuned
// Gaussian-ish that adds to ~1.0 (cheap to inline as 5 vec3
// literals; Phoskia converter accepts `vec3` array literals).
// Edge-tap clamping (out-of-bounds → use the boundary sample) is
// implicit: `sample(src, uv)` returns the clamped texel on most
// backends, matching S1a's sky / extract FS convention.
//
// Uniform gates (cutsheet lessons §3.1): all scalars as vec4 + .x
// to satisfy bgfx Vec4 upload ABI. direction + texelSize are vec2
// semantics padded into vec4 (.xy used, .zw zero). UV.y flipped
// for D3D RT vs backbuffer convention (mirror BloomExtract FS).
//
// Branchless: converter drops if/for. The 5-tap sum uses 4
// `mad` chains — no control flow. Source-image / ping-image
// binding is the SAME `texture2d source` slot; the host binds
// different textures per submit (RT0 of BloomExtract for pass A,
// RT0 of BloomBlurA for pass B).
constexpr const char* kBloomBlurPhoskiaSource = R"(
material BloomBlur {
    texture2d source
    uniform vec4 direction
    uniform vec4 texelSize
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let dir = direction.xy
        let tSize = texelSize.xy
        // 5-tap Karplus-Strong-ish Gaussian (sums to 1.0).
        // Offset = 0 (center) + 4 symmetric taps at multiples of texel.
        let c = sample(source, uv)
        let t1 = sample(source, uv + dir * tSize * 1.0)
        let t2 = sample(source, uv + dir * tSize * 2.0)
        let t3 = sample(source, uv + dir * tSize * 3.0)
        let t4 = sample(source, uv + dir * tSize * 4.0)
        // Weighted sum is already vec4 — do NOT wrap as vec4(result, c.w)
        // (HLSL rejects float4(float4, float)). Keep rgb from the blur,
        // preserve center-sample alpha.
        let result = c * 0.227
                   + t1 * 0.194 + sample(source, uv - dir * tSize * 1.0) * 0.194
                   + t2 * 0.121 + sample(source, uv - dir * tSize * 2.0) * 0.121
                   + t3 * 0.054 + sample(source, uv - dir * tSize * 3.0) * 0.054
                   + t4 * 0.016 + sample(source, uv - dir * tSize * 4.0) * 0.016
        return vec4(result.x, result.y, result.z, c.w)
    }
}
)";

// §S1b — cache-key bump forces re-acquire after shader fix.
constexpr const char* kBloomBlurCacheKey = "bloomblur_v1_separable_5tap_fs";

} // namespace

// §S1b (2026-07-23) — cache-key externalize (Bug fix #3 mirror —
// see SkyboxPass.cpp:64-68 for the originating pattern). The
// header's `extern const char* const kBloomBlurCacheKeyCStr` is
// bound to this literal so tests can `assert(kBloomBlurCacheKeyCStr
// == mirror)` and drift breaks immediately. MUST live at file
// scope inside the `ayt::render::detail` namespace so the extern
// declaration in BloomBlurPass.h finds it.
const char* const kBloomBlurCacheKeyCStr = kBloomBlurCacheKey;

uint32_t BloomBlurPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;

    // Mirror S1a BloomExtractPass + PostProcessPass / ShadowPass /
    // LightingPass / SkyboxPass — Noop + uninit short-circuits must
    // come FIRST so headless tests run clean. The FBO create path
    // inside the FG resolve() (F6) would otherwise race against
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

    // §F3 (2026-07-24) — FrameGraph must be wired (post-F2 contract).
    // Legacy callers (Pre-F2 test sites) never set frameGraph ⇒
    // early-return 0 (mirror BloomExtractPass F2 contract). The
    // PostProcessPass consumer-side wiring (S1c) still uses
    // `ctx.bloomBlurPass` borrowed ptr for its own producer
    // lookup, but this Pass's own resource resolution goes via FG
    // now. F5 will move PostProcessPass's bloom/blur sampler reads
    // to FG resolveSemantic too.
    if (ctx.frameGraph == nullptr) {
        return 0;
    }

    // Half-res size — (W+1)/2 rounds UP so we never sample outside
    // [0,W) on the source texture (mirror BloomExtract / S1 §S1).
    // Pre-F6 we compute halfW/halfH locally — F6 will replace this
    // with a FG-backed physical size getter. With resolvePingPong
    // returning invalid in the F3-skeleton phase, the same
    // early-return below triggers on the uninitialized adapter,
    // so this computation is unused today but correctly expresses
    // the post-F6 shape.
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }
    const uint16_t halfW = static_cast<uint16_t>((viewportWidth  + 1u) / 2u);
    const uint16_t halfH = static_cast<uint16_t>((viewportHeight + 1u) / 2u);

    // §F3 (2026-07-24) — Resolve BOTH ping-pong FBOs from the
    // FrameGraph instead of asking `this` to keep them. The two
    // resources are declared distinct (BloomBlurA / BloomBlurB)
    // so FG physically creates (or will physically create, in F6)
    // two separate transient RTs and **must not alias them**
    // (alias rules: F6 interval `lastRead < nextFirstWrite` keeps
    // H-write-A and V-read-A-write-B in the right order).
    //
    // On Noop / uninitialized adapter the resolvePingPong returns
    // {invalid, invalid} and the pass early-returns 0 (K2 #1
    // propagated through FG). On a future F6-compiled pipeline
    // that didn't include BloomBright (bloomEnabled=false ⇒ no
    // BloomExtract pass registered ⇒ BloomBright not live ⇒ no
    // consumer of BloomBlurA/B ⇒ FG compile culls them ⇒ resolve
    // returns {invalid, invalid} ⇒ this pass returns 0 with ZERO
    // allocations — K2 invariant #4 "关效果即不分配 RT").
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
    // when FG lazily recreates the RT (F6 size change).
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
    _fullscreenVB = adapter.createVertexBuffer(kFullscreenTriangle,
                                                sizeof(kFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kFullscreenIndices,
                                              sizeof(kFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void BloomBlurPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    // Cache-key bump forces re-acquire (pointer-equal compare).
    // S1b ships one cache-key; future cuts bump it.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kBloomBlurCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = kBloomBlurCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    ayt::shader::ShaderResource acquired =
        pool.acquire(kBloomBlurPhoskiaSource, kBloomBlurCacheKey);
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
    _sourceRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _pingRt   = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
}

} // namespace ayt::render::detail
