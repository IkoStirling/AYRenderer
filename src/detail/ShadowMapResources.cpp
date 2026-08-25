#include "detail/ShadowMapResources.h"
#include "detail/RasterConvention.h"

#include "AYRenderer/ShadowDiagnostics.h"

#include <cstdio>

namespace ayt::render::detail
{

uint64_t ShadowMapResources::casterDrawState() noexcept
{
    return BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
         | BGFX_STATE_DEPTH_TEST_LESS | kCullBackFaces;
}

void ShadowMapResources::cacheColorAttachment(BGFXAdapter& adapter)
{
    _color = BGFX_INVALID_HANDLE;
    if (!BGFXAdapter::isValid(_fbo)) {
        return;
    }
    _color = adapter.getFboAttachment(_fbo, 0);
}

bgfx::TextureHandle ShadowMapResources::colorAttachment(BGFXAdapter& adapter) const
{
    if (!BGFXAdapter::isValid(_fbo)) {
        return BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_color)) {
        return _color;
    }
    return adapter.getFboAttachment(_fbo, 0);
}

void ShadowMapResources::bindShadowView(BGFXAdapter& adapter,
                                        uint8_t viewId,
                                        uint16_t mapSize)
{
    adapter.setViewFrameBuffer(viewId, _fbo);
    adapter.setViewRect(viewId, 0, 0, mapSize, mapSize);
    // RGBA8 clear → 1.0 far in .r; depth clear 1.0 (D3D far plane).
    adapter.setViewClearRaw(viewId,
                            BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                            /*rgba=*/0xffffffff,
                            /*depth=*/1.0f,
                            /*stencil=*/0);
}

bool ShadowMapResources::resolveForSampling(BGFXAdapter& adapter, uint8_t resolveViewId)
{
    _lastBlitOk = false;
    const bgfx::TextureHandle color = colorAttachment(adapter);
    if (!BGFXAdapter::isValid(color) || !BGFXAdapter::isValid(_resolve) || _size == 0) {
        return false;
    }
    // Dedicated resolve view (not the caster view). Optional path for
    // readback tools — FO samples _color directly.
    adapter.setViewFrameBuffer(resolveViewId, BGFX_INVALID_HANDLE);
    adapter.setViewRect(resolveViewId, 0, 0, _size, _size);
    // §P4 L7 (2026-08-24) — CLEAR_NONE doesn't read the
    // depth/stencil args. The `1.0f, 0` trailer was a leftover
    // from copy-paste of the caster-view CLEAR_COLOR|DEPTH
    // pattern. Drop them for clarity (matches the Part 3
    // SkyboxPass clear-none cleanup).
    adapter.setViewClearRaw(resolveViewId, BGFX_CLEAR_NONE);
    _lastBlitOk = adapter.blitTexture(resolveViewId, _resolve, color, _size, _size);
    return _lastBlitOk;
}

void ShadowMapResources::ensureResolve(BGFXAdapter& adapter,
                                       uint16_t size,
                                       const char* buildStamp)
{
    const bool stampChanged = _buildStamp != buildStamp;
    if (stampChanged) {
        _buildStamp = buildStamp;
    }
    if (BGFXAdapter::isValid(_resolve) && _size == size && !stampChanged) {
        return;
    }
    if (BGFXAdapter::isValid(_resolve)) {
        adapter.destroy(_resolve);
        _resolve = BGFX_INVALID_HANDLE;
    }
    _resolve = adapter.createBlitDstTexture2D(size, size);
    if (!BGFXAdapter::isValid(_resolve)) {
        static uint32_t s_resolveFailLog = 0;
        // §P4 L4 (2026-08-24) — use the ShadowDiagnostics
        // `kProbeLogLimit` constant instead of the magic `2`.
        // Matches the rate-limited convention used across the
        // shadow subsystem (ShadowCaster uses
        // kVerboseLogLimit = 8).
        if (s_resolveFailLog
            < ayt::render::ShadowDiagnostics::kProbeLogLimit) {
            std::fprintf(stderr,
                         "[ShadowMapResources] resolve tex create failed %ux%u "
                         "(FO samples color RT directly; blit optional)\n",
                         static_cast<unsigned>(size),
                         static_cast<unsigned>(size));
            ++s_resolveFailLog;
        }
    }
}

void ShadowMapResources::ensure(BGFXAdapter& adapter,
                                uint16_t size,
                                const char* buildStamp)
{
    if (!adapter.isInitialized() || size == 0) {
        // §P4 M4 (2026-08-24) — rate-limited diagnostic on the
        // silent early-return. The previous shape returned
        // without logging; an editor session starting with bgfx
        // not initialized would silently skip shadow FBO
        // creation forever. Mirror the Part 3 rate-limited
        // pattern: first frame logs full reason.
        static uint32_t s_ensureSkipLog = 0;
        if (s_ensureSkipLog < 4) {
            std::fprintf(stderr,
                         "[ShadowMapResources] ensure() skipped: "
                         "initialized=%d size=%u\n",
                         adapter.isInitialized() ? 1 : 0,
                         static_cast<unsigned>(size));
            ++s_ensureSkipLog;
        }
        return;
    }

    const bool stampChanged = _buildStamp != buildStamp;
    if (stampChanged) {
        _buildStamp = buildStamp;
    }
    if (BGFXAdapter::isValid(_fbo) && _size == size && !stampChanged) {
        if (!BGFXAdapter::isValid(_color)) {
            cacheColorAttachment(adapter);
        }
        return;
    }

    if (BGFXAdapter::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo   = BGFX_INVALID_HANDLE;
        _color = BGFX_INVALID_HANDLE;
        _size  = 0;
        _lastBlitOk = false;
    }

    _fbo = adapter.createColorDepthFrameBuffer(size, size);
    if (BGFXAdapter::isValid(_fbo)) {
        _size = size;
        cacheColorAttachment(adapter);
    } else {
        // §P4 M5 (2026-08-24) — rate-limited diagnostic on
        // FBO-create failure. The previous shape was silent;
        // ShadowPass already logged "FBO create failed at
        // %ux%u" but ShadowMapResources itself gave no
        // signal. Mirror the Part 3 rate-limited pattern.
        static uint32_t s_fboCreateFailLog = 0;
        if (s_fboCreateFailLog
            < ayt::render::ShadowDiagnostics::kProbeLogLimit) {
            std::fprintf(stderr,
                         "[ShadowMapResources] color+depth FBO create "
                         "failed at %ux%u (capsTextureBlit=%d, "
                         "capsTextureReadBack=%d)\n",
                         static_cast<unsigned>(size),
                         static_cast<unsigned>(size),
                         adapter.capsTextureBlit() ? 1 : 0,
                         adapter.capsTextureReadBack() ? 1 : 0);
            ++s_fboCreateFailLog;
        }
    }
    ensureResolve(adapter, size, buildStamp);
}

void ShadowMapResources::destroy(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_resolve)) {
        adapter.destroy(_resolve);
        _resolve = BGFX_INVALID_HANDLE;
    }
    // _color is owned by _fbo (destroyTextures=true) — do not destroy it.
    _color = BGFX_INVALID_HANDLE;
    if (BGFXAdapter::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo  = BGFX_INVALID_HANDLE;
        _size = 0;
    }
    _lastBlitOk = false;
    _buildStamp = "";
}

} // namespace ayt::render::detail
