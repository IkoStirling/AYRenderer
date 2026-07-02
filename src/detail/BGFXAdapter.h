#pragma once

#include "AYRenderTypes.h"

#include <bgfx/bgfx.h>

#include <cstdint>

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

    void beginFrame();
    void endFrame();

    void setViewRect(uint8_t viewId, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void setViewClear(uint8_t viewId, const ClearDesc& clear);
    void setViewTransform(uint8_t viewId, const float* view, const float* proj);

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

    void destroy(bgfx::VertexBufferHandle h);
    void destroy(bgfx::IndexBufferHandle h);
    void destroy(bgfx::TextureHandle h);

    static bgfx::RendererType::Enum mapBackend(Backend backend);

private:
    bool _initialized = false;
};

} // namespace ayt::render::detail
