#include "detail/PostProcessPass.h"

#include "detail/GpuResources.h"

#include "AYShaderResource.h"

#include <bgfx/bgfx.h>

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

// R5.1 (2026-07-20) — Phoskia source for the post-process effect.
// Inlined as a constexpr string rather than read from disk because
// (a) ShaderResourcePool::acquire(src, cacheKey) takes the source
// directly (mirrors how unlit.phoskia is loaded in
// Test_ForwardOpaque.cpp:84 + Test_TransparentPass_U1.cpp:67), and
// (b) keeps the .phoskia artifact co-located with the only TU that
// compiles it — no fragile "where does the file live at run-time?"
// landmine. The shader does three things:
//   1) Vertex: passthrough NDC position (fullscreen triangle verts
//      are already in clip space) + emit UV varying via pos.xy * 0.5
//      + 0.5 (NDC → texture UV).
//   2) Fragment: sample u_sceneColor at UV, multiply by u_exposure,
//      add bloom-style `raw * u_bloomStrength` (R5.1 ships the math
//      path even though there's no bloom buffer yet — when the
//      downsample/upsample pair lands in R5.1.2, the host swaps
//      `raw` for the bloomed buffer in C++ without re-compiling).
//   3) Tonemap dispatch via int mode: 0 = None (passthrough), 1 =
//      Reinhard (x/(x+1)), 2 = ACES (Narkowicz fit). We use a
//      nested `if (cond) { return ... }` shape rather than
//      branchless `mix` because Phoskia's semantic analyzer accepts
//      IfStmt at fragment-body top level and this keeps the math
//      legible to anyone debugging the effect later.
constexpr const char* kPostProcessPhoskiaSource = R"(
material PostProcess {
    texture2d sceneColor
    uniform float bloomStrength
    uniform float exposure
    uniform int tonemapMode
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let sampled = sample(sceneColor, vUv)
        let raw = vec3(sampled.x, sampled.y, sampled.z) * exposure
        let withBloom = raw + vec3(bloomStrength) * raw
        if (tonemapMode == 0) {
            return vec4(withBloom, sampled.w)
        } else {
            if (tonemapMode == 1) {
                let reinhard = withBloom / (withBloom + vec3(1.0))
                return vec4(reinhard, sampled.w)
            } else {
                if (tonemapMode == 2) {
                    let a = withBloom * vec3(2.51) + vec3(0.03)
                    let b = withBloom * vec3(2.43) + vec3(0.59)
                    let c = withBloom * vec3(0.14) + vec3(0.10)
                    let aces = clamp((a * a) / (a * b + c), vec3(0.0), vec3(1.0))
                    return vec4(aces, sampled.w)
                } else {
                    return vec4(withBloom, sampled.w)
                }
            }
        }
    }
}
)";

constexpr const char* kPostProcessCacheKey = "postprocess_blit_r51";

} // namespace

uint32_t PostProcessPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;
    const uint8_t viewId = ctx.viewId;
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

    (void)viewId;

    // P2 (PR-D, 2026-07-20) — the post-process source FBO. We
    // borrow the shared scene FBO from PassExecContext when it's
    // valid (Renderer built it ⇒ ForwardOpaque/Transparent drew into
    // it ⇒ it carries the real scene color). When the FBO is invalid
    // (Noop backend, SceneRT-off host, ensureSceneFbo failed this
    // frame) we fall back to the pre-PR-D self-FBO path so headless
    // tests + backbuffer-only hosts keep working unchanged.
    const bgfx::FrameBufferHandle sceneFbo = ctx.sceneFbo;
    const bool                      hasSceneFbo = BGFXAdapter::isValid(sceneFbo);

    // P2 — resolve the program, FBOs, and quad lazily so the
    // headless test path short-circuits at the earliest possible
    // point. We always need the fullscreen quad + program ready
    // for the blit-back, so do those regardless of source.
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
        && _tSceneColor    != ayt::shader::InvalidBinding;

    // P2 — pick the source color attachment. Preferred: borrow the
    // scene FBO attach0 (this is what ForwardOpaque + Transparent
    // just drew into). Fallback: the pre-PR-D self-FBO attach0
    // (still wired, but never carries the real scene color; exists
    // so the program path remains exercisable on hosts without a
    // sceneFbo).
    bgfx::FrameBufferHandle sourceFbo = sceneFbo;
    if (!hasSceneFbo) {
        ensureFbo(adapter, viewportWidth, viewportHeight);
        if (!BGFXAdapter::isValid(_fbo)) {
            return 0;
        }
        sourceFbo = _fbo;
    }

    // R5+ (Pass-side backfill, 2026-07-20) — bind the source FBO
    // color attachment as a sampler so the post-process fragment
    // can `sample(u_sceneColor, vUv)`.
    const bgfx::TextureHandle fboColor = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(fboColor)) {
        return 0;
    }

    // P2 — bind the source FBO as the draw target. We re-bind the
    // SAME viewId on purpose: bgfx treats setViewFrameBuffer as a
    // view-scoped binding that overrides earlier per-view setView*
    // calls for that view. The downstream UIPass then rebinds view 2
    // (its own kViewId), which is unaffected. This matches
    // ForwardOpaque / Transparent's viewId choice (the scene view).
    adapter.setViewFrameBuffer(viewId, sourceFbo);
    adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);

    // R5+ (Pass-side backfill) — full 5-arg view clear through the
    // adapter. We only clear the depth when the source was the
    // pre-PR-D self-FBO (which was just created / resized and never
    // written yet); for the scene FBO we keep the depth ForwardOpaque
    // wrote so a future DepthOfField pass can read it.
    if (!hasSceneFbo) {
        adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                                /*rgba=*/0x00000000,
                                /*depth=*/1.0f,
                                /*stencil=*/0);
    }

    // R5.1 — wire the post-process knobs into the fragment shader.
    // The 4 bindings were resolved in ensureProgram() once; we look
    // them up by cached ID here. The Phoskia `uniform int tonemapMode`
    // is 4 bytes on the GLSL side, matching our int32_t cast.
    if (programReady) {
        const ayt::shader::TextureHandle texHandle =
            ayt::render::detail::toShaderTexture(fboColor);
        const float bloomStrength = frame.bloomStrength;
        const float exposure      = frame.exposure;
        const int32_t tonemapMode = static_cast<int32_t>(frame.tonemapMode);
        _program.setTexture(0, _tSceneColor, texHandle);
        _program.setUniform(_uBloomStrength, &bloomStrength, sizeof(bloomStrength));
        _program.setUniform(_uExposure,      &exposure,      sizeof(exposure));
        _program.setUniform(_uTonemapMode,   &tonemapMode,   sizeof(tonemapMode));

        // P2 — the FBO-targeted draw. Bypasses depth writes since
        // there's no depth buffer involved here; the fullscreen
        // triangle is always at the far plane.
        ayt::shader::DrawCallContext sub;
        sub.viewId = viewId;
        sub.state  = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_DEPTH_TEST_ALWAYS;
        _program.submit(sub);

        // P2 / R5.1 — blit-back to the default backbuffer. Re-bind
        // view to backbuffer (not the FBO), re-set the texture +
        // uniforms (bgfx cleared the pending list on the prior submit),
        // and re-submit. Same fullscreen-triangle path so no extra
        // GPU state. UIPass (view 2) draws cleanly over this output
        // because we never touched view 2 here.
        adapter.setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);
        adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
        adapter.setTransformIdentity();
        adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
        adapter.setIndexBuffer(_fullscreenIB, 0, 3);

        _program.setTexture(0, _tSceneColor, texHandle);
        _program.setUniform(_uBloomStrength, &bloomStrength, sizeof(bloomStrength));
        _program.setUniform(_uExposure,      &exposure,      sizeof(exposure));
        _program.setUniform(_uTonemapMode,   &tonemapMode,   sizeof(tonemapMode));
        _program.submit(sub);
        return 2;  // FBO-target draw + backbuffer-target draw
    }

    // R5+ (Pass-side backfill) — fallback no-program path. Submit
    // through the adapter so bgfx::submit doesn't leak into the
    // Pass file. Restore the default backbuffer so UIPass can
    // composite chrome over the unfiltered scene color. Return 1
    // to record the bgfx::submit that fired (the program is invalid
    // so the GPU effectively records the call but discards the
    // actual draw).
    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    adapter.submit(viewId,
                   bgfx::ProgramHandle{BGFX_INVALID_HANDLE},
                   /*depth=*/0,
                   BGFX_DISCARD_NONE);
    adapter.setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);
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
    // BGFXAdapter. Previously this called bgfx::copy +
    // bgfx::createVertexBuffer + bgfx::createIndexBuffer directly;
    // BGFXAdapter already owns both helpers (and owns the handle
    // lifecycle contract). The handle wrapper invokes bgfx::copy()
    // internally — so callers never see `bgfx::Memory*` either.
    _fullscreenVB = adapter.createVertexBuffer(kFullscreenTriangle,
                                                sizeof(kFullscreenTriangle),
                                                bgfx::VertexLayout{},
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kFullscreenIndices,
                                              sizeof(kFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void PostProcessPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    if (_program.isValid()) {
        return;
    }
    // R5.1 — pool.acquire takes the source string + a cache key.
    // On success we resolve the 4 binding IDs once (cheaper than
    // re-resolving every frame); on failure _program stays invalid
    // and execute() falls back to the R5+ no-shader path (1 draw,
    // no effect). The pool may also surface compile errors via
    // lastCompileErrors() — we log them to stderr so the failure is
    // diagnosable from a test run, but don't crash (mirrors how
    // createMaterialFromPhoskia is treated in tests: invalid
    // material = test SKIPs).
    ayt::shader::ShaderResource acquired =
        pool.acquire(kPostProcessPhoskiaSource, kPostProcessCacheKey);
    if (!acquired.isValid()) {
        std::fprintf(stderr,
                     "[PostProcessPass] Phoskia acquire failed; "
                     "post-process will run as R5+ no-op shader path\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[PostProcessPass]   %s\n", err.c_str());
        }
        return;
    }
    _program        = acquired;
    _uBloomStrength = _program.getUniformBinding("bloomStrength");
    _uExposure      = _program.getUniformBinding("exposure");
    _uTonemapMode   = _program.getUniformBinding("tonemapMode");
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
    _tSceneColor    = ayt::shader::InvalidBinding;
    _fboWidth = 0;
    _fboHeight = 0;
}

} // namespace ayt::render::detail
