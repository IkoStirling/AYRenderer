#include "detail/LightingPass.h"

namespace ayt::render::detail
{

LightingPass::~LightingPass() = default;

uint32_t LightingPass::execute(PassExecContext& ctx)
{
    // §P5 B3 (2026-07-22) — empty shell. Mirrors ShadowPass's Noop
    // early-exit (ShadowPass.cpp:24-26): if the adapter is
    // uninitialized OR the backend is the test-only Noop, return 0
    // draws without touching any GPU state.
    //
    // B5 will replace this with:
    //   - _lightingFbo.ensure(adapter, _requestedSize, ...) →
    //     BGFXAdapter::createFrameBuffer (single RGBA8 color, viewport
    //     size — LightingPass uses 1 color FBO, NOT MRT like GBuffer)
    //   - fullscreen triangle VS dispatch on view 8 (vertexLayoutPosUv
    //     from P6.5)
    //   - FS that samples ctx.gbufferPass->gbufferAlbedoRt() /
    //     gbufferNormalRt() / gbufferMotionRt() + ctx.shadowPass->shadowFbo()
    //   - 1 directional light from FrameContext::lightDirection / lightColor
    if (!ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()) {
        return 0;
    }

    // B3 ships nothing else — no FBO ensure, no draws, no stats.
    return 0;
}

void LightingPass::setOutputSize(uint16_t width, uint16_t height) noexcept
{
    _lightingW = width;
    _lightingH = height;
}

void LightingPass::destroyResources(BGFXAdapter& adapter)
{
    // B3 stub — no resources to destroy yet. B5 will mirror the
    // ShadowPass::destroyResources shape: destroy the FBO + reset
    // to BGFX_INVALID_HANDLE so the next ensure() rebuilds cleanly.
    (void)adapter;
}

} // namespace ayt::render::detail