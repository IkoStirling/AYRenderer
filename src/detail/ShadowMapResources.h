#pragma once

#include "detail/BGFXAdapter.h"

#include <bgfx/bgfx.h>

#include <cstdint>

namespace ayt::render::detail
{

// Phase 2 — RGBA8 encoded-depth FBO + D24S8/D32F depth.
// FO samples the color attachment after the caster view finishes (bgfx
// unbinds the RT between views). A separate blit→resolve path is kept
// for optional readback / tools, but must NOT gate sampling: a failed or
// empty blit left resolve at 0 → pure-black debug / crushed lit shading.
class ShadowMapResources {
public:
    void ensure(BGFXAdapter& adapter, uint16_t size, const char* buildStamp);
    void destroy(BGFXAdapter& adapter);

    bool isValid() const noexcept { return BGFXAdapter::isValid(_fbo); }
    bool hasSampleableShadow() const noexcept
    {
        return BGFXAdapter::isValid(_color);
    }
    bool lastBlitOk() const noexcept { return _lastBlitOk; }
    uint16_t allocatedSize() const noexcept { return _size; }

    bgfx::FrameBufferHandle frameBuffer() const noexcept { return _fbo; }
    bgfx::TextureHandle resolveTexture() const noexcept { return _resolve; }
    // Prefer this for FO/Transparent binds (live color RT, post-caster view).
    bgfx::TextureHandle sampleTexture() const noexcept { return _color; }

    bgfx::TextureHandle colorAttachment(BGFXAdapter& adapter) const;

    void bindShadowView(BGFXAdapter& adapter, uint8_t viewId, uint16_t mapSize);

    // Optional: copy attachment 0 → resolve on a dedicated resolveViewId.
    // Must NOT reuse the caster view (last setViewFrameBuffer wins per view).
    bool resolveForSampling(BGFXAdapter& adapter, uint8_t resolveViewId);

    // bgfx example 16: color encodes ndc01; hardware depth orders draws.
    static uint64_t casterDrawState() noexcept;

private:
    void ensureResolve(BGFXAdapter& adapter, uint16_t size, const char* buildStamp);
    void cacheColorAttachment(BGFXAdapter& adapter);

    bgfx::FrameBufferHandle _fbo     = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     _color   = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     _resolve = BGFX_INVALID_HANDLE;
    uint16_t                _size    = 0;
    bool                    _lastBlitOk = false;
    const char*             _buildStamp = "";
};

} // namespace ayt::render::detail
