#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>

namespace ayt::render::detail
{

class BGFXAdapter;

// Reusable immutable fullscreen-triangle geometry for post-process nodes.
// Each pass owns a small instance so its lifetime remains explicit; all
// allocation, binding and partial-failure cleanup behavior is centralized.
class FullscreenPassGeometry final {
public:
    bool ensure(BGFXAdapter& adapter);
    bool isReady() const noexcept;
    void bind(BGFXAdapter& adapter) const;
    void destroy(BGFXAdapter& adapter);

private:
    bgfx::VertexBufferHandle _vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle _indexBuffer = BGFX_INVALID_HANDLE;
};

// Configure either an offscreen post-process target or the final backbuffer
// presentation rect. Fullscreen shaders operate directly in NDC.
void configureFullscreenPassView(BGFXAdapter& adapter,
                                 uint8_t viewId,
                                 bgfx::FrameBufferHandle target,
                                 uint16_t x,
                                 uint16_t y,
                                 uint16_t width,
                                 uint16_t height);

} // namespace ayt::render::detail
