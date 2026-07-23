#include "detail/DepthHazePass.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <cstdio>

namespace ayt::render::detail
{

// §S4a (2026-07-23, short-term-plan §S4 sub-cut 1) — skeleton
// implementation. execute() unconditionally returns 0 (no FBO, no
// program, no draw). S4b lands the actual haze shader + the FBO
// ensure + the Deferred worldPos / Forward FS-recon branching.
//
// Why this compiles and tests-pass clean:
//   - Zero FBO allocation (K3 invariant #2: hazeEnabled=false ⇒
//     no ensureFbo ⇒ no view id collision).
//   - Zero program acquire (so shaderc absent on CI is fine).
//   - Zero bgfx state mutation.
//   - One-shot stderr log on first execute() so the host can verify
//     the pass landed in the pipeline slot table and is being
//     visited per-frame (mirror BloomExtractPass first-execute
//     log pattern).
//
// Noop-backend safety: explicit guard. When the Noop backend is
// active (headless tests), we still log + return 0 (mirror
// BloomExtractPass + PostProcessPass).

uint32_t DepthHazePass::execute(PassExecContext& ctx)
{
    // R5+ — Noop-backend / uninitialized-adapter short-circuit.
    // Mirror BloomExtractPass + PostProcessPass + ShadowPass +
    // GBufferPass + LightingPass + SkyboxPass dual guard. Every
    // BGFXAdapter FBO method gates on isInitialized, so we don't
    // need a separate "if Noop skip" branch — but we DO need an
    // early-out here so we don't log noise on every frame of the
    // headless test path (Test_DepthHaze_S4a will assert isReady()
    // == false post-execute).
    if (!ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()) {
        return 0;
    }

    // S4a: viewport-zero guard (mirror BloomExtractPass). S4b may
    // add real checks here (viewport sanity, hazeEnabled flag from
    // FrameContext, etc.) — keep this guard for the skeleton.
    if (ctx.viewportWidth == 0 || ctx.viewportHeight == 0) {
        return 0;
    }

    // §S4a (2026-07-23) — one-shot diagnostic so the host can
    // confirm DepthHazePass landed in the pipeline slot table and
    // is being visited each frame. Mirrors BloomExtractPass's
    // first-execute fprintf. Logged at INFO density (no env flag
    // gate) because the pass is opt-in via the RenderPassSlot
    // table — if the slot is mounted, the host expects to see a
    // pass-did-visit line at least once.
    static bool s_loggedFirstExecute = false;
    if (!s_loggedFirstExecute) {
        std::fprintf(stderr,
            "[DepthHazePass] §S4a skeleton — view=%u rect=(%u,%u,%u,%u) "
            "halfResFbo=invalid (S4b lands the FBO ensure + shader); "
            "execute() returns 0; PostProcessPass haze sampler not "
            "wired yet (lands in §S4c)\n",
            static_cast<unsigned>(kDepthHazeViewId),
            static_cast<unsigned>(ctx.viewportX),
            static_cast<unsigned>(ctx.viewportY),
            static_cast<unsigned>(ctx.viewportWidth),
            static_cast<unsigned>(ctx.viewportHeight));
        s_loggedFirstExecute = true;
    }

    // S4a: zero-draw early return. K3 invariant #2 says
    // hazeEnabled=false ⇒ no FBO ensure; since S4a doesn't ship
    // ensureFbo at all, the invariant is trivially satisfied (no
    // FBO is ever created). S4b will add the real ensureFbo call
    // gated on FrameContext::hazeEnabled.
    return 0;
}

void DepthHazePass::ensureFbo(BGFXAdapter& /*adapter*/,
                              uint16_t      /*viewportW*/,
                              uint16_t      /*viewportH*/)
{
    // §S4a skeleton — no-op. S4b lands the real RGBA8 half-res FBO
    // creation gated on hazeEnabled (frame.hazeEnabled=true). When
    // the flag is false, ensureFbo is a no-op (K3 invariant #2: no
    // allocation when disabled). When true, mirrors
    // BloomExtractPass::ensureFbo contract: lazy create + resize on
    // viewport change.
    return;
}

void DepthHazePass::ensureFullscreenQuad(BGFXAdapter& /*adapter*/)
{
    // §S4a skeleton — no-op. S4b mirrors BloomExtractPass:
    // BGFXAdapter::vertexLayoutPosUv() + createVertexBuffer +
    // createIndexBuffer.
    return;
}

void DepthHazePass::ensureProgram(shader::ShaderResourcePool& /*pool*/)
{
    // §S4a skeleton — no-op. S4b mirrors BloomExtractPass: cache-key
    // bump + pool.acquire() with kDepthHazeCacheKey extern const
    // (mirror BloomExtract's extern cache-key pattern used by
    // unit tests for live-drift detection).
    return;
}

void DepthHazePass::destroyResources(BGFXAdapter& /*adapter*/)
{
    // §S4a skeleton — no-op (no resources to release). S4b mirrors
    // BloomExtractPass::destroyResources: invalidate FBO + VB + IB,
    // reset program, reset binding IDs, reset acquire latch.
    return;
}

} // namespace ayt::render::detail