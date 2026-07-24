#include "detail/GBufferDebugPass.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <cstdio>

namespace ayt::render::detail
{

namespace {

// V1 placeholder Phoskia source — body is a no-op `return vec4(0,0,0,1)`.
// V2 replaces this with the real per-channel Phoskia FS (Albedo
// direct / Normal remap / WorldPos direct / Motion alias-V1-or-real-
// RT-V2 / Depth linearize). The placeholder is intentionally NOT
// compiled in V1 — `ensureProgram()` only runs in V2 (V1 execute
// returns 0 at step 4 because the host-owned debug FBO is invalid
// for the default-off case, and at step 8 because the program is
// invalid even when the host enabled the pass).
constexpr const char* kGBufferDebugPhoskiaSource = R"(
material GBufferDebug {
    texture2d albedo
    texture2d normal
    texture2d worldPos
    texture2d depthTex
    uniform vec4 debugChannel
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        return vec4(0.0, 0.0, 0.0, 1.0)
    }
}
)";

// V1 cache-key — placeholder string. V2 bumps to the real per-
// channel Phoskia cache key (mirror SSAO A3 v0→v3→v4 bump).
constexpr const char* kGBufferDebugCacheKey = "gbufferdebug_v1_skeleton_fs";

} // namespace

// Bug-fix-#3 mirror: file-scope external definition (same pattern as
// kSSAOCacheKeyCStr at SSAOPass.cpp:208). Tests pin via extern
// declaration in GBufferDebugPass.h:181-182.
const char* const kGBufferDebugCacheKeyCStr = kGBufferDebugCacheKey;

uint32_t GBufferDebugPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;

    // Mirror SSAOPass.cpp:210-225 early-return ladder. V1 K-GBD-1
    // is enforced here: every short-circuit returns 0 BEFORE any
    // RT/program access. V2 keeps these guards and adds the
    // per-channel sampler bind + Phoskia submit.
    if (!adapter.isInitialized()) {
        return 0;
    }
    if (adapter.isNoopBackend()) {
        return 0;
    }

    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;
    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }

    // V1 K-GBD-1 step-4 — host-owned debug FBO. Invalid ⇒
    // gbufferDebugEnabled was false at the central gate ⇒ no
    // allocation ⇒ 0 draw.
    const bgfx::FrameBufferHandle target = ctx.gbufferDebugFbo;
    if (!BGFXAdapter::isValid(target)) {
        return 0;
    }

    // V1 K-GBD-1 — Deferred-only MVP. The host's central gate
    // also gates on gbufferPassPtr != nullptr, so this is a
    // double-check (cutsheet §5.5 redundancy rule).
    if (ctx.gbufferPass == nullptr) {
        return 0;
    }

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensureProgram(ctx.pool);
    const bool programReady = _program.isValid()
        && _uDebugChannel != ayt::shader::InvalidBinding
        && _tAlbedo       != ayt::shader::InvalidBinding
        && _tNormal       != ayt::shader::InvalidBinding
        && _tWorldPos     != ayt::shader::InvalidBinding
        && _tDepth        != ayt::shader::InvalidBinding;
    if (!programReady) {
        // V1 stub: the placeholder Phoskia source is intentionally
        // never compiled (the cache-key is also a placeholder; V2
        // will lift the acquire path). Returning 0 here is
        // byte-equivalent to the V1 default-off case — the host's
        // FBO stays untouched, no draws, no allocs.
        return 0;
    }

    // V2: view 250 wire + per-channel sampler bind + uniform
    // upload + fullscreen submit. V1 dead code path.
    constexpr uint8_t viewId = kGBufferDebugViewId;
    adapter.setViewFrameBuffer(viewId, target);
    adapter.setViewRect(viewId, 0, 0, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, ctx.frame.view, ctx.frame.projection);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_COLOR, 0x000000FF, 1.0f, 0);

    // V2: per-channel Phoskia FS. The 4 texture inputs (albedoRt
    // / normalRt / motionRt-as-worldPos / depthRt) are bound
    // unconditionally; the channel uniform selects which one
    // survives. V1 returns 0 before this block.

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;
    adapter.setStateDepthTestAlways();
    _program.submit(sub);

    static bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        std::fprintf(stderr,
            "[GBufferDebugPass] V2 first dispatch view=%u "
            "viewport=%ux%u enabled=%d channel=%u\n",
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(viewportWidth),
            static_cast<unsigned>(viewportHeight),
            ctx.frame.gbufferDebugEnabled ? 1 : 0,
            static_cast<unsigned>(ctx.frame.gbufferDebugChannel));
        s_loggedFirst = true;
    }
    return 1;
}

void GBufferDebugPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    // V1 ships without lazy-allocating the fullscreen quad. V2
    // fills in the kFullscreenTriangle / kFullscreenIndices
    // mirrors (copy from SSAOPass.cpp:22-35). The early return at
    // the `isValid` check above keeps V1 byte-equivalent to the
    // pre-V1 state (no draws, no allocs).
    (void)adapter;
}

void GBufferDebugPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kGBufferDebugCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = kGBufferDebugCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }

    // V1 intentionally does NOT call pool.acquire() — the
    // placeholder Phoskia source above is a skeleton, not the
    // real per-channel FS. V2 will replace this with the actual
    // acquire + binding resolution block. We set
    // _programAcquireFailed = false so V2 can run the real
    // acquire path without tripping the latch.
    (void)pool;
    _programAcquireFailed = true;
}

void GBufferDebugPass::destroyResources(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    if (_program.isValid()) {
        _program.reset();
    }
    _uDebugChannel   = ayt::shader::InvalidBinding;
    _tAlbedo         = ayt::shader::InvalidBinding;
    _tNormal         = ayt::shader::InvalidBinding;
    _tWorldPos       = ayt::shader::InvalidBinding;
    _tDepth          = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
}

} // namespace ayt::render::detail
