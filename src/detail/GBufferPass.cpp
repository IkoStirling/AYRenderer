#include "detail/GBufferPass.h"

namespace ayt::render::detail
{

GBufferPass::~GBufferPass() = default;

uint32_t GBufferPass::execute(PassExecContext& ctx)
{
    // §P5 B2 (2026-07-22) — empty shell. Mirrors ShadowPass's Noop
    // early-exit (ShadowPass.cpp:24-26): if the adapter is
    // uninitialized OR the backend is the test-only Noop, return 0
    // draws without touching any GPU state.
    //
    // B4 will replace this with:
    //   - _gbufferFbo.ensure(adapter, _requestedSize, ...) →
    //     BGFXAdapter::createGbufferFrameBuffer (new MRT helper per
    //     docs/pass-lessons-from-deferred.md §5.2)
    //   - per-DrawItem gbuffer-fill VS/FS dispatch on view 7
    //   - depth-write + no-blend state (BGFX_STATE_WRITE_RGB |
    //     WRITE_A | WRITE_Z | DEPTH_TEST_LESS)
    //
    // B5's LightingPass will consume ctx.gbufferPass->gbufferFbo()
    // + the 3 RT attachments as its scene-color/normal/motion
    // inputs.
    if (!ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()) {
        return 0;
    }

    // B2 ships nothing else — no FBO ensure, no draws, no stats.
    // Returning 0 here keeps the existing Pipeline::executeAll
    // draw-count sum contract intact (the rest of the pipeline
    // runs as before).
    return 0;
}

void GBufferPass::setGbufferSize(uint16_t width, uint16_t height) noexcept
{
    // B2 stub — preserves the requested size for B4 to honor when
    // ensure() lands. No GPU work; B4 will detect a size mismatch
    // with `_gbufferW / _gbufferH` and rebuild the MRT.
    _gbufferW = width;
    _gbufferH = height;
}

void GBufferPass::destroyResources(BGFXAdapter& adapter)
{
    // B2 stub — no resources to destroy yet. B4 will mirror the
    // ShadowPass::destroyResources shape: destroy the FBO + the 3
    // RT attachments + reset to BGFX_INVALID_HANDLE so the next
    // ensure() rebuilds cleanly.
    (void)adapter;
}

} // namespace ayt::render::detail