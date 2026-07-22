#include "detail/ShadowMapResources.h"

#include <cstdio>

namespace ayt::render::detail
{

uint64_t ShadowMapResources::casterDrawState() noexcept
{
    return BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z
         | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CCW;
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
    adapter.setViewClearRaw(resolveViewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
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
        if (s_resolveFailLog < 2) {
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
