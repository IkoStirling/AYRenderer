#include "detail/GBufferPass.h"

namespace ayt::render::detail
{

// §P5 B4a (2026-07-22) — build stamp literal (mirror
// ShadowMapResources.h:55). Pointer-equal comparison; callers MUST
// pass this exact literal. Bumping requires a string change here AND
// a re-ensure (next execute() will rebuild the FBO).
static constexpr const char* kGBufferBuildStamp = "b4a-2026-07-22";

GBufferPass::~GBufferPass() = default;

uint32_t GBufferPass::execute(PassExecContext& ctx)
{
    // §P5 B4a (2026-07-22) — shell evolved: ensure 4-attach MRT FBO
    // + cache attachments. Still NO draw dispatch (B4c territory,
    // depends on B4b Phoskia GBuffer VS/FS which is deferred per
    // cutsheet §4.2 because Phoskia converter does not support
    // fragment multi-output — see `AYShader/design.md:2645-2655`).
    //
    // B5 LightingPass will consume gbufferFbo() + the 3 RT
    // attachments as its scene-color/normal/motion inputs.
    if (!ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()) {
        return 0;
    }

    // Disable signal: host called setGbufferSize(0, 0) (or never
    // called setGbufferSize and we have no default we should
    // implicitly apply). Mirror ShadowMapResources::ensure early-
    // return on size == 0 (ShadowMapResources.cpp:97-99).
    if (_gbufferW == 0 || _gbufferH == 0) {
        return 0;
    }

    ensure(ctx.adapter, _gbufferW, _gbufferH);
    return 0;  // B4a: no draw dispatch yet (B4c territory)
}

void GBufferPass::setGbufferSize(uint16_t width, uint16_t height) noexcept
{
    // B4a: still only stores request (mirror B2 behavior — do NOT
    // call ensure here; no adapter access). Host can call this BEFORE
    // initialize() and the next execute() will honor the size.
    // B2 case 4 (`setGbufferSize(1920,1080)` then `gbufferFbo() invalid`)
    // stays green because no FBO work happens here.
    _gbufferW = width;
    _gbufferH = height;
}

void GBufferPass::destroyResources(BGFXAdapter& adapter)
{
    // B4a: real cleanup (mirror ShadowMapResources::destroy
    // ShadowMapResources.cpp:128-143). All 4 attachments are owned
    // by _gbufferFbo (destroyTextures=true upstream), so resetting
    // them to BGFX_INVALID_HANDLE is enough — DO NOT call
    // bgfx::destroy on the cached attachments or you'll double-free.
    //
    // W/H + buildStamp are reset unconditionally (Test_B4_GBufferMRT
    // case 6 verifies gbufferWidth/Height return 0 after destroy even
    // when no FBO was ever allocated — a host that calls
    // setGbufferSize(800,600) → destroyResources() expects W/H back to
    // 0). The FBO handle itself is only destroyed when it was actually
    // allocated (calling bgfx::destroy on an invalid handle is a UAF
    // on some bgfx backends — see ShadowMapResources::destroy guard).
    _gbufferDepthRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferAlbedoRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferNormalRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferMotionRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferW = 0;
    _gbufferH = 0;
    _buildStamp = "";
    if (bgfx::isValid(_gbufferFbo)) {
        adapter.destroy(_gbufferFbo);
        _gbufferFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
}

void GBufferPass::ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    // Mirror ShadowMapResources::ensure (ShadowMapResources.cpp:93-126).
    //
    // Stamp-changed fast path: when `buildStamp` differs from
    // `_buildStamp`, force rebuild. Currently we always pass
    // kGBufferBuildStamp so stamp is stable across frames — keeps
    // the cache hot until a cutsheet-bump forces a rebuild.
    if (!adapter.isInitialized() || width == 0 || height == 0) {
        return;
    }

    const bool stampChanged = (_buildStamp != kGBufferBuildStamp);
    if (stampChanged) {
        _buildStamp = kGBufferBuildStamp;
    }

    // Fast path: same FBO, same size, same stamp — just re-cache
    // attachments if any went stale (e.g., post `bgfx::reset`).
    if (bgfx::isValid(_gbufferFbo)
        && _gbufferW == width
        && _gbufferH == height
        && !stampChanged) {
        if (!bgfx::isValid(_gbufferAlbedoRt)) {
            cacheAttachments(adapter);
        }
        return;
    }

    // Rebuild path — destroy old FBO + reset cache + create new.
    if (bgfx::isValid(_gbufferFbo)) {
        adapter.destroy(_gbufferFbo);
        _gbufferFbo       = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _gbufferAlbedoRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferNormalRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferMotionRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferDepthRt   = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferW = _gbufferH = 0;
    }

    _gbufferFbo = adapter.createGbufferFrameBuffer(width, height);
    if (bgfx::isValid(_gbufferFbo)) {
        _gbufferW = width;
        _gbufferH = height;
        cacheAttachments(adapter);
    }
}

void GBufferPass::cacheAttachments(BGFXAdapter& adapter)
{
    // Mirror ShadowMapResources::cacheColorAttachment
    // (ShadowMapResources.cpp:14-21). All 4 attachments are owned by
    // _gbufferFbo (destroyTextures=true), so we read them via
    // adapter.getFboAttachment and never call bgfx::destroy on them.
    _gbufferAlbedoRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferNormalRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferMotionRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferDepthRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    if (!bgfx::isValid(_gbufferFbo)) {
        return;
    }
    _gbufferAlbedoRt = adapter.getFboAttachment(_gbufferFbo, 0);
    _gbufferNormalRt = adapter.getFboAttachment(_gbufferFbo, 1);
    _gbufferMotionRt = adapter.getFboAttachment(_gbufferFbo, 2);
    _gbufferDepthRt  = adapter.getFboAttachment(_gbufferFbo, 3);
}

} // namespace ayt::render::detail