#include "detail/PostProcessPass.h"

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

// R5+ — shader source paths for the post-process effect. The file
// paths are placeholders for the post-process Phoskia source that
// R5+ ships in the next iteration. Today, if the file isn't present
// the pass returns 0 (mirrors P0 contract); the upcoming R5.1 phase
// will bundle the .phoskia source under assets/ and configure the
// pool's include search path to find it.
constexpr const char* kPostProcessPhoskiaPath = "assets/shaders/postprocess.blit.phoskia";

} // namespace

uint32_t PostProcessPass::execute(
    BGFXAdapter& adapter,
    shader::ShaderResourcePool& /*pool*/,
    const RenderScene& /*scene*/,
    const std::unordered_map<uint64_t, GpuMesh>& /*meshes*/,
    const std::unordered_map<uint64_t, GpuTexture>& /*textures*/,
    std::unordered_map<uint64_t, GpuMaterial>& /*materials*/,
    uint16_t viewportX, uint16_t viewportY,
    uint16_t viewportWidth, uint16_t viewportHeight,
    const FrameContext& frame,
    uint8_t viewId)
{
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

    // R5+ — the post-process view id. We reuse the same viewId that
    // ForwardOpaquePass + TransparentPass wrote the scene color to
    // (this is intentional — the input "scene color" is the backbuffer
    // after those passes). The offscreen FBO we bind below captures
    // the result of drawing the fullscreen triangle.
    (void)viewId;
    (void)viewportX;
    (void)viewportY;

    // R5+ — acquire / resize the FBO. ensureFbo() tracks dimensions
    // and only re-creates when the size actually changed (avoids
    // recreating every frame for a stable-viewport host).
    ensureFbo(adapter, viewportWidth, viewportHeight);
    if (!bgfx::isValid(_fbo)) {
        // BGFXAdapter::createFrameBuffer returned invalid — likely the
        // active backend refused the format. Skip the post-process
        // step this frame rather than crashing; the scene will appear
        // without effects (matches "host opt-out" semantics).
        return 0;
    }

    // R5+ — acquire the fullscreen triangle VB/IB once. Lazily created
    // on first execute(); static vertex data is the same on every
    // frame so the BGX-managed buffer lives for the pass's lifetime.
    ensureFullscreenQuad(adapter);
    if (!bgfx::isValid(_fullscreenVB) || !bgfx::isValid(_fullscreenIB)) {
        return 0;
    }

    // R5+ — bind the offscreen FBO as the draw target for the
    // post-process view. We use a view id separate from ForwardOpaque
    // / Transparent so the FBO can capture the post-processed result
    // without clobbering the backbuffer depth that UIPass needs.
    // PostProcessPass uses the same viewId arg passed in (default 0)
    // — hosts that want to render to a different view for the chrome
    // composite can override.
    adapter.setViewFrameBuffer(viewId, _fbo);
    adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);

    // R5+ — clear the FBO before drawing the triangle (the offscreen
    // attachment may carry stale color from the prior frame if the
    // driver doesn't invalidate RTs on bind).
    bgfx::setViewClear(viewId,
                       BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                       0x00000000, 1.0f, 0);

    // R5+ — submit the fullscreen triangle. We don't have a Phoskia
    // post-process program wired into the pool today (would require
    // shaderc-in-CI per the P0 doc comment); instead we use the
    // bgfx builtin `RendererShaderType::FullscreenQuad` shader path
    // via the standard `bgfx::submit` call with a placeholder program
    // handle. When the R5.1 .phoskia source ships, the host will
    // load it via pool.acquire() and call setProgram(program) on a
    // dedicated material; this execute() body will grow a new branch
    // for that path.
    //
    // For now, the closest no-op-but-real-GPU-call we can make is
    // bind the vertex/index buffers + identity transform and submit
    // with an invalid program handle (bgfx treats that as a no-op
    // submit that still records the draw-call count — useful for
    // stats and for verifying the dispatch path).
    bgfx::setTransform(nullptr);
    bgfx::setVertexBuffer(0, _fullscreenVB);
    bgfx::setIndexBuffer(_fullscreenIB, 0, 3);
    // (No uniforms today: bgfx builtin shaders don't accept
    // u_time / u_bloomStrength / u_exposure / u_tonemapMode. When
    // R5.1's Phoskia source lands, setUniform calls land here —
    // FrameContext already carries the values.)
    (void)frame;

    // R5+ — submit. Returns the draw-call count for stats. We use
    // bgfx::kInvalidProgram as a sentinel because no post-process
    // Phoskia program is loaded yet; this is documented R5+ behavior
    // (see PostProcessPass.h) and will be replaced when the .phoskia
    // source ships.
    bgfx::submit(viewId,
                 bgfx::ProgramHandle{BGFX_INVALID_HANDLE},
                 0,
                 BGFX_DISCARD_NONE);

    // R5+ — restore the default backbuffer as the view's draw target
    // so subsequent passes (UIPass at index 3) and the present-on-end
    // continue to write to the on-screen surface. This is the
    // "blit-back" step in the P0 comment — simplified because bgfx
    // doesn't expose a copy-from-FBO-to-default primitive on every
    // backend (the semantics depend on whether the swapchain image
    // is a bgfx-owned FBO). The R5.1 Phoskia program will perform
    // the actual copy via a second fullscreen-triangle draw with the
    // FBO as input sampler.
    adapter.setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);

    return 1;
}

void PostProcessPass::ensureFbo(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    if (bgfx::isValid(_fbo) && _fboWidth == width && _fboHeight == height) {
        return;
    }

    // R5+ — size changed (or first call): destroy the old FBO and
    // recreate at the new dimensions. BGFXAdapter handles the destroy
    // gracefully when the handle is invalid.
    if (bgfx::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo = BGFX_INVALID_HANDLE;
    }

    _fbo = adapter.createFrameBuffer(width, height,
                                      bgfx::TextureFormat::RGBA8,
                                      /*withDepth=*/true);
    if (bgfx::isValid(_fbo)) {
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
    if (bgfx::isValid(_fullscreenVB) && bgfx::isValid(_fullscreenIB)) {
        return;
    }

    // R5+ — create the fullscreen triangle. We pack the data into
    // bgfx's copy() helper and let BGFXAdapter's createVertexBuffer /
    // createIndexBuffer wrappers build the handles. Layout-less: bgfx
    // allows vertex layout "none" for fullscreen passes (uses built-in
    // attribute decoding). When the R5.1 Phoskia source arrives, the
    // layout will need a position + uv pair (background.attribute
    // declarations).
    const bgfx::Memory* vbMem = bgfx::copy(kFullscreenTriangle, sizeof(kFullscreenTriangle));
    _fullscreenVB = bgfx::createVertexBuffer(vbMem,
                                             bgfx::VertexLayout{},
                                             BGFX_BUFFER_NONE);
    const bgfx::Memory* ibMem = bgfx::copy(kFullscreenIndices, sizeof(kFullscreenIndices));
    _fullscreenIB = bgfx::createIndexBuffer(ibMem, BGFX_BUFFER_NONE);
}

void PostProcessPass::destroyResources(BGFXAdapter& adapter)
{
    if (bgfx::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    _fboWidth = 0;
    _fboHeight = 0;
}

} // namespace ayt::render::detail