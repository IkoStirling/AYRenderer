#include "detail/SSAOPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/FgResource.h"        // §A1 SSAO MVP (2026-07-24) — FrameGraph SSAOTexture resolve gate
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"

#include "AYShaderResource.h"

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// §A1 (2026-07-24) — fullscreen-triangle layout mirror
// (DepthHazePass.cpp:25-38 / BloomExtractPass). Three vertices with
// x,y on clip-space corners + u,v on the [0,1] UV plane (UV.y flip
// handled in FS for D3D RT vs backbuffer convention). A3 will use
// this to draw onto the SSAOTexture FBO.
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

// §A1 (2026-07-24) — Cache-key placeholder. A3 lifts this to the
// real 8-tap worldPos-sphere shader cache key on first execute
// when the SSAO Phoskia source ships. Pre-A3 the literal is just
// a sentinel so the extern-from-test drift-detection can validate
// the file is wired up. Bumping it is mandatory when A3 lands (a
// stale cache key would skip the new source and the unit-test
// cache-key mirror would still match, giving false green).
constexpr const char* kSSAOCacheKey = "ssao_v0_skeleton_placeholder";

} // namespace

// §A1 (2026-07-24) — cache-key externalize (mirror DepthHazePass /
// BloomExtractPass / BloomBlurPass / SkyboxPass / LightingPass
// Bug-fix-#3 pattern). The header's `extern const char* const
// kSSAOCacheKeyCStr` is bound to this literal so tests can
// `assert(kSSAOCacheKeyCStr == mirror)` and drift breaks
// immediately. MUST live at file scope inside the
// `ayt::render::detail` namespace so the extern declaration in
// SSAOPass.h finds it.
const char* const kSSAOCacheKeyCStr = kSSAOCacheKey;

uint32_t SSAOPass::execute(PassExecContext& ctx)
{
    // §A1 SSAO MVP skeleton — early-return 0 for every guard below.
    // The real GPU work (8-tap sphere) is the A3 commit. A1 only
    // ships the contract, the enum / view id reservation, and the
    // 0-draw guard. A3 fills in:
    //   1) Phoskia program acquire (cache-key bump + ensureProgram)
    //   2) noise texture lazy upload + ensureNoise
    //   3) worldPos RT read via ctx.gbufferPass->gbufferMotionRt()
    //   4) sample 8 sphere taps with viewProjectionMatrix*vec4
    //   5) worldPos.w > 0 sky reject via step(0.0001, w)
    //   6) clamp(1 - pow(1 - occFraction, 4), 0, 1) final AO
    //   7) write to ctx.frameGraph->resolve(SSAOTexture)

    // Mirror the FG-bypass contract that DepthHazePass F4 and
    // PostProcessPass F5 use: when the upstream FrameGraph is
    // not wired (legacy callers / pre-F2 test sites), early-return
    // 0 — the pass is harmless. The render() central ssaoPassEnabled
    // gate decides whether SSAOTexture even reaches live set; if
    // not, resolve(SSAOTexture) below returns invalid and we exit
    // again at line ~88.
    if (ctx.frameGraph == nullptr) {
        return 0;
    }

    // §K-SSAO-1 (2026-07-24) — The canonical "this pass should run"
    // signal in the FG era. The host's knob is folded into the
    // render() central `ssaoPassEnabled` boolean: when false, FG
    // compile culls SSAOTexture ⇒ resolve returns invalid ⇒ 0
    // draw, 0 alloc. Mirror DepthHazePass F4 + BloomExtractPass F2.
    const bgfx::FrameBufferHandle target =
        ctx.frameGraph->resolve(FgResourceId::SSAOTexture);
    if (!BGFXAdapter::isValid(target)) {
        return 0;
    }

    // §A3 placeholder — A1 stops here. The A3 real-implementation
    // will land below this line (ensureFullscreenQuad →
    // ensureProgram → ensureNoise → bind RT, sampler, uniforms →
    // submit draw). For now, return 0 so K-SSAO-1 holds: even when
    // SSAOTexture is "live" in the graph, A1 sends no draw call.

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        std::fprintf(stderr,
            "[SSAOPass] A1 skeleton: 0 draw, view=%u Fbo=%u "
            "(real impl lands in A3).\n",
            static_cast<unsigned>(kSsaoViewId),
            static_cast<unsigned>(target.idx));
        s_loggedFirst = true;
    }
    return 0;
}

void SSAOPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    // Mirror DepthHazePass::ensureFullscreenQuad (line 305). A3 will
    // call this from execute(). A1 keeps it inline since the current
    // execute() early-returns before reaching the call site.
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    _fullscreenVB = adapter.createVertexBuffer(kFullscreenTriangle,
                                                sizeof(kFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kFullscreenIndices,
                                              sizeof(kFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void SSAOPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    // A3 placeholder body — A1 ships with no real shader. Latching
    // logic mirrors DepthHazePass::ensureProgram (line 325-334) so
    // the cache-key bump contract is in place for A3. A1 leaves
    // _program invalid and `_programAcquireFailed = false` because
    // we never actually attempt acquire in A1 (execute() returns
    // early). A3 will call this and gate on the result.
    (void)pool;
}

void SSAOPass::ensureNoise(BGFXAdapter& adapter)
{
    // A3 placeholder — 4×4 RGBA8 tangent-rotation noise table,
    // uploaded once via BGFXAdapter::createTexture2D. A1 keeps
    // _noiseTex invalid so the destroy path is a clean no-op.
    (void)adapter;
}

void SSAOPass::destroyResources(BGFXAdapter& adapter)
{
    // §A1 (2026-07-24) — Skeleton only. A3 will mirror
    // DepthHazePass::destroyResources (line 360-389) and add the
    // _noiseTex release. A1 has nothing to free; calling destroy
    // twice is a safe no-op (mirror PostProcessPass / DepthHazePass
    // idempotent contract).
    (void)adapter;
    _program.reset();
    _uSSAOStrength  = ayt::shader::InvalidBinding;
    _uSSAORadius    = ayt::shader::InvalidBinding;
    _uSSAOBias      = ayt::shader::InvalidBinding;
    _uProjection    = ayt::shader::InvalidBinding;
    _tSceneColor    = ayt::shader::InvalidBinding;
    _tWorldPosition = ayt::shader::InvalidBinding;
    _tNoise         = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
}

} // namespace ayt::render::detail
