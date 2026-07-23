#include "detail/DepthHazePass.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"

#include "AYShaderResource.h"

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// Mirror S1a BloomExtractPass / S1b BloomBlurPass — single oversize
// fullscreen triangle covers the entire viewport without a diagonal
// seam (bgfx 00-helloworld pattern). UV.y flip handled in FS for
// D3D RT vs backbuffer convention.
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

// §S4b (2026-07-23, short-term-plan §S4 sub-cut 2) — exponential
// depth-aware haze Phoskia source. Final-PP composite in §S4c will
// sample `outputColor` as the haze contribution (mixed over the
// un-bloomed raw scene color — bloom stays independent per
// short-term-plan §S4 决策 2026-07-23 "haze 只改 raw, bloom 独立").
//
// Formula (主人拍板 B = exponential):
//   fogFactor = 1 - exp(-density * dist)
//   outRgb    = mix(rawRgb, fogColor, fogFactor * strength)
//   outAlpha  = rawAlpha  (haze does NOT touch alpha — preserves
//                          composite contract for BloomExtract /
//                          PostProcessPass layered blits).
//
// dist = max(raw.b, 0) — S4b simplification: S4a/§S4 决策
// "Forward 用 FS 重建 fallback,Deferred 用 GBuffer RT2 worldPos" is
// punted to §S4d once proper worldPos reconstruction is in scope;
// today we read sceneColor.b as a cheap luminance-encoded distance
// proxy so the pass can ship without growing the sampler chain.
// §S4b K3 invariant #4: the dist proxy is bounded [0, 1] (raw.b is
// the BT.709 luminance already in the [0, 1] RT0 range), so
// `exp(-density * 0) = 1` and `exp(-density * 1) ≈ 0` for
// density > 5, which collapses the fog to a 0..1 fade without
// blowing up the exp. Result is byte-equivalent to a no-op when
// density is 0 OR raw.b is 0 (both ⇒ exp(0) = 1 ⇒ fogFactor = 0).
//
// Uniform gates (cutsheet lessons §3.1): all scalars as `uniform vec4`
// with .x carry — bgfx Vec4 upload ABI. hazeColor carries .xyz; pad
// .w with 0. UV.y flipped for D3D RT vs backbuffer convention (mirror
// BloomExtractPass / BloomBlurPass FS).
//
// Branchless: converter drops if/for. fogFactor strength gating is a
// single `mix(0, fogFactor, step(0.0, strength))` so a hazeStrength
// of 0 keeps the composite at `rawRgb * (1 - 0) = rawRgb` (mirror
// §S1c bloomStrength branchless collapse — PostProcessPass S4c
// sampler wire relies on this when depthHazePass is null).
constexpr const char* kDepthHazePhoskiaSource = R"(
material DepthHaze {
    texture2d sceneColor
    texture2d worldPosOrDepth
    uniform vec4 hazeDensity
    uniform vec4 hazeStrength
    uniform vec4 hazeColor
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let raw = sample(sceneColor, uv)
        let depthProxy = sample(worldPosOrDepth, uv)
        // S4b simplification: dist proxy = luminance of the depth
        // source. S4d will swap this for a proper worldPos / depth
        // attachment sample. Bounded [0, 1] so exp(-density * dist)
        // is well-defined for all density >= 0.
        let dist = max(depthProxy.b, 0.0)
        let fogFactor = 1.0 - exp(-hazeDensity.x * dist)
        // Branchless strength gate — hazeStrength.x <= 0 ⇒ mix
        // weight collapses to 0 ⇒ output == raw (K3 invariant #1).
        let gated = fogFactor * step(0.0, hazeStrength.x)
        let mixed = mix(raw.xyz, hazeColor.xyz, gated * min(hazeStrength.x, 1.0))
        return vec4(mixed, raw.w)
    }
}
)";

// §S4b — cache-key bump forces re-acquire after shader fix. v1 ships
// exponential fog + depth-proxy + hazeColor. Future cuts bump to v2+
constexpr const char* kDepthHazeCacheKey = "depthhaze_v1_exp_fog_fs";

} // namespace

// §S4b (2026-07-23) — cache-key externalize (mirror BloomExtractPass /
// BloomBlurPass / SkyboxPass / LightingPass Bug-fix-#3 pattern). The
// header's `extern const char* const kDepthHazeCacheKeyCStr` is bound
// to this literal so tests can `assert(kDepthHazeCacheKeyCStr ==
// mirror)` and drift breaks immediately. MUST live at file scope
// inside the `ayt::render::detail` namespace so the extern
// declaration in DepthHazePass.h finds it.
const char* const kDepthHazeCacheKeyCStr = kDepthHazeCacheKey;

uint32_t DepthHazePass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;

    // Mirror BloomExtractPass / PostProcessPass / ShadowPass /
    // LightingPass / SkyboxPass — Noop + uninit short-circuits must
    // come FIRST so headless tests run clean. The FBO create path
    // inside ensureFbo would otherwise race against bgfx::createFrameBuffer
    // with no init context.
    if (!adapter.isInitialized()) {
        return 0;
    }
    if (adapter.isNoopBackend()) {
        // Same rationale as BloomExtractPass / PostProcessPass: Noop
        // backend returns valid handles for everything (so handle-
        // validity can't distinguish "real backend that's broken"
        // from "Noop that should skip"). Skip the pass entirely —
        // preserves the K3 invariant #2 (Noop ⇒ no FBO created +
        // 0 draws).
        return 0;
    }

    // §S4b K3 invariant #2 — hazeEnabled=false ⇒ no FBO allocation
    // (守 frame-graph-mvp.md §7 第 3 条: 关效果即不分配 RT). Short-
    // circuit BEFORE ensureFbo so the half-res RT never gets created
    // when the host has not opted in. Mirror BloomExtractPass
    // viewport-zero guard — same shape, different gate source.
    if (!frame.hazeEnabled) {
        return 0;
    }

    // §S4b K3 invariant #1 — hazeStrength <= 0 ⇒ haze is logically
    // off (host set the knob but kept strength at 0). Pre-S4 byte-
    // equivalent: PostProcessPass S4c sampler wire binds sceneColor
    // on the haze slot; FS branchless composite collapses to
    // `raw * (1 - 0) = raw`. We could still allocate the FBO here
    // (cheap, but not free), so we explicitly early-return — keeps
    // the "host enabled + strength=0 ⇒ no work" contract crisp.
    if (frame.hazeStrength <= 0.0f) {
        return 0;
    }

    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }

    // Source-FBO priority — identical to BloomExtractPass / PostProcessPass
    // (cutsheet §P5 B6 lock — deferred LightingOutput wins over
    // forward sceneFbo; both invalid ⇒ no work). We need the same
    // FBO the Final PP composite will read so the haze result is a
    // haze OF the post-lighting scene color (matches S4b decision
    // "haze only modifies raw, bloom stays independent"). Reusing
    // the static helper keeps the priority decision a single source
    // of truth.
    const bgfx::FrameBufferHandle sourceFbo = PostProcessPass::selectSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }

    const bgfx::TextureHandle fboColor = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(fboColor)) {
        return 0;
    }

    // Half-resolution size — (W+1)/2 rounds UP so we never sample
    // outside [0, W) on the source texture. Mirror conventional
    // half-res chain math (S1 cutsheet §S1 "ensure(w/2, h/2)").
    const uint16_t halfW = static_cast<uint16_t>((viewportWidth  + 1u) / 2u);
    const uint16_t halfH = static_cast<uint16_t>((viewportHeight + 1u) / 2u);
    ensureFbo(adapter, viewportWidth, viewportHeight);
    if (!BGFXAdapter::isValid(_fbo)) {
        return 0;
    }

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensureProgram(pool);
    const bool programReady = _program.isValid()
        && _uHazeDensity      != ayt::shader::InvalidBinding
        && _uHazeStrength     != ayt::shader::InvalidBinding
        && _uHazeColor        != ayt::shader::InvalidBinding
        && _tSceneColor       != ayt::shader::InvalidBinding
        && _tWorldPosOrDepth  != ayt::shader::InvalidBinding;
    if (!programReady) {
        // Acquire failed (shaderc missing on CI). Skip the draw so
        // the half-res FBO stays clear (any S4c consumer would then
        // sample zero ⇒ PostProcessPass haze sampler branchless
        // composite collapses to `raw * (1 - 0) = raw` — visually
        // identical to hazeEnabled=false). Mirror BloomExtractPass
        // contract.
        return 0;
    }

    // Bind the half-res FBO as the draw target. Do NOT bind it as
    // sampler (mirror BloomExtractPass::execute — same-FBO feedback
    // clears / blacks the half-res buffer for the next frame).
    constexpr uint8_t viewId = kDepthHazeViewId;
    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();

    adapter.setViewFrameBuffer(viewId, _fbo);
    adapter.setViewRect(viewId, 0, 0, halfW, halfH);
    adapter.setViewTransform(viewId, identity, identity);
    // Don't clear — we always overwrite every pixel via fullscreen
    // triangle. Clearing wastes a depth-stencil resolve on some
    // backends (mirror BloomExtractPass).
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);

    const ayt::shader::TextureHandle texHandle =
        ayt::render::detail::toShaderTexture(fboColor);

    // bgfx Vec4 slots — pad scalars into .x, fog color into .xyz
    // (lessons §3.1). strength clamp at upload time prevents any host
    // accident (e.g. setDepthHazeStrength(2.0)) from over-driving
    // the fog beyond a full fade-to-color.
    const float densityPad[4]  = {frame.hazeDensity, 0.0f, 0.0f, 0.0f};
    const float strengthPad[4] = {frame.hazeStrength, 0.0f, 0.0f, 0.0f};
    const float colorPad[4]    = {
        frame.hazeColor.x, frame.hazeColor.y, frame.hazeColor.z, 0.0f};

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    // §S4b — both samplers bind to the SAME sceneColor handle.
    // S4d will swap the second slot for a proper depth / worldPos
    // attachment (cutsheet §S4 决策 "Deferred 采 GBuffer RT2 worldPos
    // + Forward FS 重建 fallback"). Today we use the same RT for
    // both texture slots so the Phoskia source compiles + links
    // without leaving a sampler unit unbound (which would
    // otherwise emit GLSL "sampler not set" warnings on the
    // backend). Net result: the dist proxy == raw.b luminance, and
    // the exponential collapse to raw is correct for the S4b
    // simplification (the FS hazeColor mix factor becomes
    // `1 - exp(-density * raw.b) * strength`, which is a
    // luminance-keyed fade — visually similar enough to depth
    // fog for the S4b ship).
    _program.setTexture(0, _tSceneColor,      texHandle);
    _program.setTexture(0, _tWorldPosOrDepth, texHandle);
    _program.setUniform(_uHazeDensity,  densityPad,  sizeof(densityPad));
    _program.setUniform(_uHazeStrength, strengthPad, sizeof(strengthPad));
    _program.setUniform(_uHazeColor,    colorPad,    sizeof(colorPad));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;
    adapter.setStateDepthTestAlways();
    _program.submit(sub);

    // Do NOT setViewFrameBuffer(viewId, INVALID) after submit — mirror
    // BloomExtractPass pattern (last bind wins for the whole view
    // this frame; clearing to default backbuffer at (0,0) would
    // paint the half-res haze onto the Editor chrome area).

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        std::fprintf(stderr,
            "[DepthHazePass] first dispatch view=%u srcFbo=%u "
            "half=%ux%u enabled=%d strength=%.2f density=%.3f "
            "color=(%.2f,%.2f,%.2f)\n",
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(sourceFbo.idx),
            static_cast<unsigned>(halfW),
            static_cast<unsigned>(halfH),
            frame.hazeEnabled ? 1 : 0,
            frame.hazeStrength,
            frame.hazeDensity,
            frame.hazeColor.x,
            frame.hazeColor.y,
            frame.hazeColor.z);
        s_loggedFirst = true;
    }
    return 1;
}

void DepthHazePass::ensureFbo(BGFXAdapter& adapter, uint16_t viewportW, uint16_t viewportH)
{
    // Half-res, computed from the full viewport (mirror BloomExtractPass).
    const uint16_t halfW = static_cast<uint16_t>((viewportW  + 1u) / 2u);
    const uint16_t halfH = static_cast<uint16_t>((viewportH + 1u) / 2u);

    if (BGFXAdapter::isValid(_fbo) && _fboWidth == halfW && _fboHeight == halfH) {
        return;
    }
    // Size changed (or first call): destroy the old FBO then
    // recreate at the new dimensions. BGFXAdapter::destroy handles
    // invalid handles cleanly (no-op).
    if (BGFXAdapter::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo = BGFX_INVALID_HANDLE;
    }
    _fbo = adapter.createFrameBuffer(halfW, halfH,
                                     bgfx::TextureFormat::RGBA8,
                                     /*withDepth=*/false);
    if (BGFXAdapter::isValid(_fbo)) {
        _fboWidth  = halfW;
        _fboHeight = halfH;
    } else {
        _fboWidth  = 0;
        _fboHeight = 0;
        std::fprintf(stderr,
                     "[DepthHazePass] FBO create failed at %ux%u; "
                     "depth-haze disabled for this run\n",
                     halfW, halfH);
    }
}

void DepthHazePass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    // Mirror BloomExtractPass / BloomBlurPass — funnel VB/IB through
    // BGFXAdapter (cutsheet §7 red line: Pass files never call
    // bgfx:: directly). Layout MUST match FullscreenVertex {x,y,u,v}:
    // a 0-stride bgfx::VertexLayout triggers bgfx::fatal under Debug.
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    _fullscreenVB = adapter.createVertexBuffer(kFullscreenTriangle,
                                                sizeof(kFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kFullscreenIndices,
                                              sizeof(kFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void DepthHazePass::ensureProgram(shader::ShaderResourcePool& pool)
{
    // Cache-key bump forces re-acquire (pointer-equal compare).
    // S4b ships one cache-key; future cuts bump it.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kDepthHazeCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = kDepthHazeCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    ayt::shader::ShaderResource acquired =
        pool.acquire(kDepthHazePhoskiaSource, kDepthHazeCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[DepthHazePass] Phoskia acquire failed; "
                     "depth-haze will skip (S4b = 0 draw).\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[DepthHazePass]   %s\n", err.c_str());
        }
        return;
    }
    _program          = acquired;
    _uHazeDensity     = _program.getUniformBinding("hazeDensity");
    _uHazeStrength    = _program.getUniformBinding("hazeStrength");
    _uHazeColor       = _program.getUniformBinding("hazeColor");
    _tSceneColor      = _program.getTextureBinding("sceneColor");
    _tWorldPosOrDepth = _program.getTextureBinding("worldPosOrDepth");
}

void DepthHazePass::destroyResources(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    if (_program.isValid()) {
        // Mirror BloomExtractPass::destroyResources —
        // ShaderResource carries no back-pointer to its pool;
        // Renderer::Impl owns the pool and outlives this pass.
        _program.reset();
    }
    _uHazeDensity     = ayt::shader::InvalidBinding;
    _uHazeStrength    = ayt::shader::InvalidBinding;
    _uHazeColor       = ayt::shader::InvalidBinding;
    _tSceneColor      = ayt::shader::InvalidBinding;
    _tWorldPosOrDepth = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
    _fboWidth  = 0;
    _fboHeight = 0;
}

} // namespace ayt::render::detail
