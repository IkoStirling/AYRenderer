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

// Post-process: sceneColor sample with optional screen-space ripple.
// Branchless (converter drops if/for). All knobs are vec4 (.x) for
// bgfx Vec4 upload ABI — see docs/pass-lessons-from-shadow.md §3.1.
// rippleParams.x = strength (0 = identity UV), .y = frequency, .z = speed.
constexpr const char* kPostProcessPhoskiaSource = R"(
material PostProcess {
    texture2d sceneColor
    uniform vec4 bloomStrength
    uniform vec4 exposure
    uniform vec4 tonemapMode
    uniform vec4 uTime
    uniform vec4 rippleParams
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let center = vec2(0.5, 0.5)
        let d = vUv - center
        let dist = length(d)
        let invLen = 1.0 / max(dist, 0.0001)
        let dir = d * invLen
        let wave = sin(dist * rippleParams.y - uTime.x * rippleParams.z)
        let offset = dir * (wave * rippleParams.x)
        let uv = vUv + offset
        let sampled = sample(sceneColor, uv)
        let raw = sampled.xyz * exposure.x
        let withBloom = raw + raw * bloomStrength.x
        return vec4(withBloom, sampled.w)
    }
}
)";

constexpr const char* kPostProcessCacheKey = "postprocess_ripple_v1_vec4";

} // namespace

uint32_t PostProcessPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;
    // Own blit view — must not reuse the scene view id (would clobber
    // scene FBO binding + camera VP for every FO submit that frame).
    const uint8_t viewId = kBlitViewId;
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

    // P2 — only blit when a real scene FBO was filled this frame.
    // Editor composite passes INVALID sceneFbo (3D already in the
    // backbuffer hole); creating a local empty FBO and blitting it
    // would paint the panel black.
    const bgfx::FrameBufferHandle sceneFbo = ctx.sceneFbo;
    const bool hasSceneFbo = BGFXAdapter::isValid(sceneFbo);
    if (!hasSceneFbo) {
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
        && _uRippleParams  != ayt::shader::InvalidBinding
        && _tSceneColor    != ayt::shader::InvalidBinding;

    const bgfx::FrameBufferHandle sourceFbo = sceneFbo;
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
        // No blit program ⇒ scene stays only in the FBO. Still restore
        // the backbuffer binding so UIPass can composite chrome.
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
    const float ripplePad[4] = {
        frame.rippleStrength, frame.rippleFrequency, frame.rippleSpeed, 0.0f};

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    _program.setTexture(0, _tSceneColor, texHandle);
    _program.setUniform(_uBloomStrength, bloomPad, sizeof(bloomPad));
    _program.setUniform(_uExposure, exposurePad, sizeof(exposurePad));
    _program.setUniform(_uTonemapMode, tonemapPad, sizeof(tonemapPad));
    _program.setUniform(_uTime, timePad, sizeof(timePad));
    _program.setUniform(_uRippleParams, ripplePad, sizeof(ripplePad));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
               | BGFX_STATE_DEPTH_TEST_ALWAYS;
    _program.submit(sub);
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
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();

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
    // R5.1 — pool.acquire takes the source string + a cache key.
    // On success we resolve the 4 binding IDs once (cheaper than
    // re-resolving every frame); on failure latch _programAcquireFailed
    // so we do not re-invoke shaderc every frame (that stuttered the
    // Editor when HLSL rejected the previous Phoskia body).
    ayt::shader::ShaderResource acquired =
        pool.acquire(kPostProcessPhoskiaSource, kPostProcessCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
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
    _uTime          = _program.getUniformBinding("uTime");
    _uRippleParams  = _program.getUniformBinding("rippleParams");
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
    _uRippleParams  = ayt::shader::InvalidBinding;
    _tSceneColor    = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
    _fboWidth = 0;
    _fboHeight = 0;
}

} // namespace ayt::render::detail
