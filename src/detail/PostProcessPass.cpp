#include "detail/PostProcessPass.h"

#include "detail/GpuResources.h"
#include "detail/GBufferPass.h"
#include "detail/LightingPass.h"

#include "AYShaderResource.h"

#include <cstdio>
#include <cstring>

namespace ayt::render::detail
{

namespace {

// R5+ — fullscreen-triangle vertex data. 3 verts in NDC: a single
// oversize triangle covers the entire screen, no index buffer needed
// past 3 indices. Using a triangle (vs. a 4-vert quad) avoids the
// diagonal seam across adjacent pixels — bgfx's fullscreen-quad
// examples use the same pattern (see bgfx/examples/00-helloworld).
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

// Post-process: sample sceneColor → exposure → optional bloom tint →
// display gamma (sRGB approx pow 1/2.2). Ripple UV warp removed.
//
// Branchless (converter drops if/for). Knobs are vec4 (.x) for bgfx
// Vec4 upload ABI — see docs/pass-lessons-from-shadow.md §3.1.
//
// UV.y flip in fragment (1 - vUv.y): Phoskia vertex blocks reject
// `let` before `out`. Same D3D RT vs backbuffer convention as shadows.
constexpr const char* kPostProcessPhoskiaSource = R"(
material PostProcess {
    texture2d sceneColor
    uniform vec4 bloomStrength
    uniform vec4 exposure
    uniform vec4 tonemapMode
    uniform vec4 uTime
    uniform vec4 gammaParams
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let sampled = sample(sceneColor, uv)
        let raw = sampled.xyz * exposure.x
        let withBloom = raw + raw * bloomStrength.x
        let cx = max(withBloom.x, 0.0)
        let cy = max(withBloom.y, 0.0)
        let cz = max(withBloom.z, 0.0)
        let invG = 1.0 / max(gammaParams.x, 0.0001)
        let encoded = vec3(pow(cx, invG), pow(cy, invG), pow(cz, invG))
        return vec4(encoded, sampled.w)
    }
}
)";

constexpr const char* kPostProcessCacheKey = "postprocess_gamma_v1_yflip_fs";

// Fallback if gamma program fails to acquire — still gamma-encodes so
// Editor composite does not go black / linear-washed.
constexpr const char* kPostProcessPassthroughSource = R"(
material PostProcessBlit {
    texture2d sceneColor
    uniform vec4 bloomStrength
    uniform vec4 exposure
    uniform vec4 tonemapMode
    uniform vec4 uTime
    uniform vec4 gammaParams
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let sampled = sample(sceneColor, uv)
        let raw = sampled.xyz * exposure.x
        let withBloom = raw + raw * bloomStrength.x
        let cx = max(withBloom.x, 0.0)
        let cy = max(withBloom.y, 0.0)
        let cz = max(withBloom.z, 0.0)
        let invG = 1.0 / max(gammaParams.x, 0.0001)
        let encoded = vec3(pow(cx, invG), pow(cy, invG), pow(cz, invG))
        return vec4(encoded, sampled.w)
    }
}
)";
constexpr const char* kPostProcessPassthroughCacheKey = "postprocess_passthrough_gamma_v1_yflip_fs";

} // namespace

uint32_t PostProcessPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;
    // Own blit view — must not reuse the scene view id (would clobber
    // scene FBO binding + camera VP for every FO submit that frame).
    // Deferred: Lighting=8, Transparent=9 → blit must be AFTER both
    // or PP samples a pre-lighting / pre-glass buffer.
    constexpr uint8_t kDeferredBlitViewId = 10;
    const bool deferredLit = (ctx.lightingPass != nullptr) &&
        bgfx::isValid(ctx.lightingPass->lightingOutputFbo());
    const uint8_t viewId = deferredLit ? kDeferredBlitViewId : kBlitViewId;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;

    // R5+ — Noop-backend short-circuit. The headless test path runs
    // execute() against a default-constructed BGFXAdapter (isInitialized
    // == false). Every BGFXAdapter FBO method gates on isInitialized,
    // so we don't need a separate "if Noop skip" branch — but we DO
    // need an early-out here so we don't allocate any resources
    // (the FBO path inside ensureFbo would otherwise race against
    // bgfx::createFrameBuffer with no init context).
    if (!adapter.isInitialized()) {
        return 0;
    }
    if (adapter.isNoopBackend()) {
        // R5+ — the Noop backend returns valid handles for everything
        // (so handle-validity checks can't distinguish "real backend
        // that's broken" from "Noop that should skip"). The shutdown
        // path under Noop is also fragile — bgfx::destroy on Noop
        // handles in a partial state has caused flakiness in the
        // Test_CaptureScreenshot path. Skip the pass entirely here:
        // it preserves the P0 contract (post-process is opt-in via
        // FrameContext knobs; default values = no-op image).
        return 0;
    }

    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }

    const uint16_t viewportX = ctx.viewportX;
    const uint16_t viewportY = ctx.viewportY;

    // §P5 B6 (2026-07-22) — source-FBO priority helper. Collapses
    // the cutsheet closure (pass-lessons-from-deferred.md:169)
    // and the P2 default into a single static. Priority:
    //   1. deferred path: ctx.gbufferPass->lightingOutputFbo()
    //      (only when LightingPass is mounted AND its FBO ensured)
    //   2. forward path: ctx.sceneFbo (P2 default)
    //   3. invalid → return 0 (no-op)
    //
    // Helper does NOT branch in execute() — same single linear flow
    // as before; only the sourceFbo value flips. No new view, no
    // new shader, no new uniform path.
    const bgfx::FrameBufferHandle sourceFbo = selectSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }
    ensureProgram(pool);
    const bool programReady = _program.isValid()
        && _uBloomStrength != ayt::shader::InvalidBinding
        && _uExposure      != ayt::shader::InvalidBinding
        && _uTonemapMode   != ayt::shader::InvalidBinding
        && _uTime          != ayt::shader::InvalidBinding
        && _uGammaParams   != ayt::shader::InvalidBinding
        && _tSceneColor    != ayt::shader::InvalidBinding;

    const bgfx::TextureHandle fboColor = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(fboColor)) {
        return 0;
    }

    // Single blit: sample source color → default backbuffer.
    // Do NOT bind sourceFbo as the draw target while sampling it
    // (same-FBO feedback clears/blacks the Editor viewport).
    //
    // View rect uses the editor panel offset (vx,vy) so the blit
    // lands in the Game View hole, not at the window origin.
    // Identity view/proj: the fullscreen triangle is already in NDC;
    // leaving the camera matrices from FO would warp it off-screen.
    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();
    adapter.setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);
    adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, identity, identity);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);

    if (!programReady) {
        static bool s_loggedMissing = false;
        if (!s_loggedMissing) {
            std::fprintf(stderr,
                "[PostProcessPass] FATAL: no blit program — scene stays in FBO "
                "(Game View may be black). Check Phoskia acquire errors above.\n");
            s_loggedMissing = true;
        }
        return 0;
    }

    const ayt::shader::TextureHandle texHandle =
        ayt::render::detail::toShaderTexture(fboColor);
    // bgfx Vec4 slots — pad scalars into .x (lessons §3.1).
    const float bloomPad[4] = {frame.bloomStrength, 0.0f, 0.0f, 0.0f};
    const float exposurePad[4] = {frame.exposure, 0.0f, 0.0f, 0.0f};
    const float tonemapPad[4] = {
        static_cast<float>(static_cast<int32_t>(frame.tonemapMode)), 0.0f, 0.0f, 0.0f};
    const float timePad[4] = {frame.timeSeconds, 0.0f, 0.0f, 0.0f};
    const float gammaPad[4] = {frame.gamma, 0.0f, 0.0f, 0.0f};

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    _program.setTexture(0, _tSceneColor, texHandle);
    _program.setUniform(_uBloomStrength, bloomPad, sizeof(bloomPad));
    _program.setUniform(_uExposure, exposurePad, sizeof(exposurePad));
    _program.setUniform(_uTonemapMode, tonemapPad, sizeof(tonemapPad));
    _program.setUniform(_uTime, timePad, sizeof(timePad));
    _program.setUniform(_uGammaParams, gammaPad, sizeof(gammaPad));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;  // P6.5: per-draw state owned by Adapter (see
                       // setStateDepthTestAlways() called below).
    // P6.5 (2026-07-22) — preset state replaces the inline
    // `BGFX_STATE_WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS`. Bit
    // combination identical.
    adapter.setStateDepthTestAlways();
    _program.submit(sub);

    static bool s_loggedSubmit = false;
    if (!s_loggedSubmit) {
        std::fprintf(stderr,
            "[PostProcessPass] blit ok view=%u rect=(%u,%u,%u,%u) "
            "gamma=%.1f exposure=%.2f time=%.2f\n",
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(viewportX),
            static_cast<unsigned>(viewportY),
            static_cast<unsigned>(viewportWidth),
            static_cast<unsigned>(viewportHeight),
            gammaPad[0],
            frame.exposure,
            frame.timeSeconds);
        s_loggedSubmit = true;
    }
    return 1;
}

void PostProcessPass::ensureFbo(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    if (BGFXAdapter::isValid(_fbo) && _fboWidth == width && _fboHeight == height) {
        return;
    }

    // R5+ — size changed (or first call): destroy the old FBO and
    // recreate at the new dimensions. BGFXAdapter handles the destroy
    // gracefully when the handle is invalid.
    if (BGFXAdapter::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo = BGFX_INVALID_HANDLE;
    }

    _fbo = adapter.createFrameBuffer(width, height,
                                      bgfx::TextureFormat::RGBA8,
                                      /*withDepth=*/true);
    if (BGFXAdapter::isValid(_fbo)) {
        _fboWidth  = width;
        _fboHeight = height;
    } else {
        _fboWidth  = 0;
        _fboHeight = 0;
        std::fprintf(stderr,
                     "[PostProcessPass] FBO create failed at %ux%u; "
                     "post-process disabled for this run\n",
                     width, height);
    }
}

void PostProcessPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }

    // R5+ (Pass-side backfill) — funnel the VB/IB creation through
    // BGFXAdapter. Layout MUST match FullscreenVertex {x,y,u,v}:
    // an empty `bgfx::VertexLayout{}` is invalid (stride 0) and
    // triggers bgfx::fatal / debugBreak under Debug builds the first
    // time PostProcess runs on a real GPU backend. TexCoord0 is
    // packed for stride alignment; the Phoskia VS currently rebuilds
    // UV from pos.xy (see kPostProcessPhoskiaSource).
    //
    // P6.5 (2026-07-22) — layout construction now goes through
    // BGFXAdapter::vertexLayoutPosUv() instead of inlining
    // bgfx::VertexLayout::begin().add(...).end() here. The
    // returned layout has the same byte shape (Position 2 floats +
    // TexCoord0 2 floats).
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();

    _fullscreenVB = adapter.createVertexBuffer(kFullscreenTriangle,
                                                sizeof(kFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kFullscreenIndices,
                                              sizeof(kFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void PostProcessPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    // Prefer the gamma blit; fall back to passthrough gamma so Editor
    // composite (FBO → backbuffer) never goes black on a bad compile.
    ayt::shader::ShaderResource acquired =
        pool.acquire(kPostProcessPhoskiaSource, kPostProcessCacheKey);
    if (!acquired.isValid()) {
        std::fprintf(stderr,
                     "[PostProcessPass] gamma Phoskia acquire failed; "
                     "trying passthrough blit\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[PostProcessPass]   %s\n", err.c_str());
        }
        acquired = pool.acquire(kPostProcessPassthroughSource,
                                kPostProcessPassthroughCacheKey);
    }
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[PostProcessPass] Phoskia acquire failed; "
                     "post-process will skip blit (scene may stay offscreen)\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[PostProcessPass]   %s\n", err.c_str());
        }
        return;
    }
    _program        = acquired;
    _uBloomStrength = _program.getUniformBinding("bloomStrength");
    _uExposure      = _program.getUniformBinding("exposure");
    _uTonemapMode   = _program.getUniformBinding("tonemapMode");
    _uTime          = _program.getUniformBinding("uTime");
    _uGammaParams   = _program.getUniformBinding("gammaParams");
    _tSceneColor    = _program.getTextureBinding("sceneColor");
}

void PostProcessPass::destroyResources(BGFXAdapter& adapter)
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
        // R5.1 — release the ShaderResource. ShaderResource doesn't
        // carry a back-pointer to its pool; the pool reference is
        // held by Renderer::Impl and outlives this pass (see
        // AYRenderer.cpp:160 shutdown order: resources → shaderPool
        // → adapter). Resetting _program here is the documented
        // ShaderResource contract (see ShaderResource::reset in
        // AYShaderResource.h:32) — the pool's dtor releases the
        // underlying GPU program once the refcount hits zero,
        // regardless of which ShaderResource instance held the last
        // reference.
        _program.reset();
    }
    _uBloomStrength = ayt::shader::InvalidBinding;
    _uExposure      = ayt::shader::InvalidBinding;
    _uTonemapMode   = ayt::shader::InvalidBinding;
    _uTime          = ayt::shader::InvalidBinding;
    _uGammaParams   = ayt::shader::InvalidBinding;
    _tSceneColor    = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
    _fboWidth = 0;
    _fboHeight = 0;
}

// §P5 B6 (2026-07-22) — source-FBO priority helper. See
// PostProcessPass.h for the priority ordering. Static, not a
// member of PostProcessPass, so tests can call it directly
// without spinning up execute() / ensure path.
bgfx::FrameBufferHandle PostProcessPass::selectSourceFbo(
    const PassExecContext& ctx) noexcept
{
    // 1) Deferred path — LightingPass is mounted AND its
    //    `lightingOutputFbo()` is valid (B5 ensure ran this frame).
    //    cutsheet pass-lessons-from-deferred.md:169 anchor.
    //    Returned through ctx.gbufferPass's borrowed pointer so
    //    PostProcessPass never owns the FBO (mirror FO/Trans
    //    ownership discipline).
    if (ctx.gbufferPass != nullptr && ctx.lightingPass != nullptr) {
        const bgfx::FrameBufferHandle lightingFbo =
            ctx.lightingPass->lightingOutputFbo();
        if (bgfx::isValid(lightingFbo)) {
            return lightingFbo;
        }
    }

    // 2) Forward path / fallback — P2 default (PR-D, 2026-07-20).
    //    Renderer-owned color+depth FBO that FO + Transparent wrote
    //    into. Validated by BGFXAdapter::isValid (cutsheet §1.7
    //    "no FBO/work" signal).
    if (BGFXAdapter::isValid(ctx.sceneFbo)) {
        return ctx.sceneFbo;
    }

    // 3) Neither valid → caller (execute) early-returns 0.
    return BGFX_INVALID_HANDLE;
}

} // namespace ayt::render::detail
