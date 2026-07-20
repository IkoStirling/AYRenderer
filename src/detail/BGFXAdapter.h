#pragma once

#include "AYRenderTypes.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>

namespace ayt::render::detail
{

struct BGFXInitParams {
    void*    nativeWindowHandle = nullptr;
    uint32_t width  = 1280;
    uint32_t height = 720;
    bool     vsync  = true;
    Backend  backend = Backend::Auto;
};

class BGFXAdapter {
public:
    BGFXAdapter() = default;

    bool initialize(const BGFXInitParams& params);
    void shutdown();

    bool isInitialized() const noexcept { return _initialized; }

    // R5+ — true when the underlying bgfx backend is Noop. Useful for
    // Pass implementations that want to skip GPU work on the headless
    // test path without breaking their lifecycle hooks (the Noop
    // backend still returns valid handles from create* so handle
    // validity alone is not a reliable "skip me" signal).
    bool isNoopBackend() const noexcept;

    void beginFrame();
    void endFrame();

    bool requestScreenshot(const std::string& filePath);

    void setViewRect(uint8_t viewId, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void setViewClear(uint8_t viewId, const ClearDesc& clear);
    void setViewClearNone(uint8_t viewId);
    void setViewTransform(uint8_t viewId, const float* view, const float* proj);

    void resetResolution(uint32_t width, uint32_t height, bool vsync);

    void setTransform(const ayt::math::Float4x4& world);
    void setVertexBuffer(bgfx::VertexBufferHandle vb, uint32_t start = 0,
                         uint32_t count = UINT32_MAX);
    void setIndexBuffer(bgfx::IndexBufferHandle ib, uint32_t start = 0,
                        uint32_t count = UINT32_MAX);

    bgfx::VertexBufferHandle createVertexBuffer(const void* data, uint32_t size,
                                                const bgfx::VertexLayout& layout,
                                                uint16_t flags = BGFX_BUFFER_NONE);
    bgfx::IndexBufferHandle createIndexBuffer(const void* data, uint32_t size,
                                              uint16_t flags = BGFX_BUFFER_NONE);

    bgfx::TextureHandle createTexture2D(uint16_t width, uint16_t height,
                                        const void* rgba8Data,
                                        uint64_t flags = BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);
    bgfx::TextureHandle createTexture2DFromData(uint16_t width, uint16_t height,
                                              bgfx::TextureFormat::Enum format,
                                              const void* data, uint32_t size,
                                              uint64_t flags = BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);

    // R5+ (Phase PostProcess, 2026-07-20) — framebuffer abstraction.
    // BGFXAdapter owns FBO handles the same way it owns VB/IB/Texture
    // handles (single source of truth for GPU resource lifecycle).
    // Reused by R5+ deferred Shadow / GBuffer / Lighting Passes
    // (design.md:457-461) — all of those need offscreen color/depth
    // attachments; building one FBO layer here keeps the ownership
    // path consistent and avoids per-Pass destructor-order landmines.
    //
    // Returns bgfx::kInvalidFrameBufferHandle if not initialized or if
    // creation fails (caller treats that as "skip post-process this
    // frame"). Tracking is internal — `destroy()` is the public teardown.
    bgfx::FrameBufferHandle createFrameBuffer(uint16_t width, uint16_t height,
                                              bgfx::TextureFormat::Enum colorFormat =
                                                  bgfx::TextureFormat::RGBA8,
                                              bool withDepth = true);

    // R5+ — bind `fb` as the draw target for `viewId`. Pass
    // bgfx::kInvalidFrameBufferHandle to revert to the default
    // backbuffer (called by PostProcessPass::execute after blitting
    // the offscreen result back, before returning).
    void setViewFrameBuffer(uint8_t viewId, bgfx::FrameBufferHandle fb);

    void destroy(bgfx::VertexBufferHandle h);
    void destroy(bgfx::IndexBufferHandle h);
    void destroy(bgfx::TextureHandle h);
    void destroy(bgfx::FrameBufferHandle h);

    static bgfx::RendererType::Enum mapBackend(Backend backend);

private:
    bool _initialized = false;
};

} // namespace ayt::render::detail
