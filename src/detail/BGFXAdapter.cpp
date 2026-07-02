#include "detail/BGFXAdapter.h"

#include "AYMathTypes.h"

namespace ayt::render::detail
{

bgfx::RendererType::Enum BGFXAdapter::mapBackend(Backend backend)
{
    switch (backend) {
    case Backend::Direct3D11:  return bgfx::RendererType::Direct3D11;
    case Backend::Direct3D12:  return bgfx::RendererType::Direct3D12;
    case Backend::Vulkan:      return bgfx::RendererType::Vulkan;
    case Backend::OpenGL:      return bgfx::RendererType::OpenGL;
    case Backend::Metal:       return bgfx::RendererType::Metal;
    case Backend::Noop:        return bgfx::RendererType::Noop;
    case Backend::Auto:
    default:                   return bgfx::RendererType::Count;
    }
}

bool BGFXAdapter::initialize(const BGFXInitParams& params)
{
    if (_initialized) {
        return true;
    }

    bgfx::Init init;
    init.type     = mapBackend(params.backend);
    init.vendorId = BGFX_PCI_ID_NONE;

    init.platformData.nwh = params.nativeWindowHandle;
#if BX_PLATFORM_LINUX
    init.platformData.ndt = nullptr;
#endif

    init.resolution.width  = params.width;
    init.resolution.height = params.height;
    init.resolution.reset  = params.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;

    if (!bgfx::init(init)) {
        return false;
    }

    _initialized = true;
    return true;
}

void BGFXAdapter::shutdown()
{
    if (!_initialized) {
        return;
    }
    bgfx::shutdown();
    _initialized = false;
}

void BGFXAdapter::beginFrame()
{
    bgfx::touch(0);
}

void BGFXAdapter::endFrame()
{
    bgfx::frame();
}

void BGFXAdapter::setViewRect(uint8_t viewId, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    bgfx::setViewRect(viewId, x, y, w, h);
}

void BGFXAdapter::setViewClear(uint8_t viewId, const ClearDesc& clear)
{
    uint16_t flags = BGFX_CLEAR_COLOR;
    if (clear.clearDepth) {
        flags |= BGFX_CLEAR_DEPTH;
    }
    const uint32_t rgba = static_cast<uint32_t>(clear.r * 255.0f) << 24
                        | static_cast<uint32_t>(clear.g * 255.0f) << 16
                        | static_cast<uint32_t>(clear.b * 255.0f) << 8
                        | static_cast<uint32_t>(clear.a * 255.0f);
    bgfx::setViewClear(viewId, flags, rgba, 1.0f, 0);
}

void BGFXAdapter::setViewTransform(uint8_t viewId, const float* view, const float* proj)
{
    bgfx::setViewTransform(viewId, view, proj);
}

void BGFXAdapter::setTransform(const ayt::math::Float4x4& world)
{
    bgfx::setTransform(world.ptr());
}

void BGFXAdapter::setVertexBuffer(bgfx::VertexBufferHandle vb, uint32_t start, uint32_t count)
{
    bgfx::setVertexBuffer(0, vb, start, count);
}

void BGFXAdapter::setIndexBuffer(bgfx::IndexBufferHandle ib, uint32_t start, uint32_t count)
{
    bgfx::setIndexBuffer(ib, start, count);
}

bgfx::VertexBufferHandle BGFXAdapter::createVertexBuffer(const void* data, uint32_t size,
                                                         const bgfx::VertexLayout& layout,
                                                         uint16_t flags)
{
    const bgfx::Memory* mem = bgfx::copy(data, size);
    return bgfx::createVertexBuffer(mem, layout, flags);
}

bgfx::IndexBufferHandle BGFXAdapter::createIndexBuffer(const void* data, uint32_t size,
                                                       uint16_t flags)
{
    const bgfx::Memory* mem = bgfx::copy(data, size);
    return bgfx::createIndexBuffer(mem, flags);
}

bgfx::TextureHandle BGFXAdapter::createTexture2D(uint16_t width, uint16_t height,
                                                  const void* rgba8Data,
                                                  uint64_t flags)
{
    if (width == 0 || height == 0 || rgba8Data == nullptr) {
        return BGFX_INVALID_HANDLE;
    }
    const bgfx::Memory* mem = bgfx::copy(rgba8Data,
                                         static_cast<uint32_t>(width) * height * 4u);
    return bgfx::createTexture2D(width, height, false, 1,
                                 bgfx::TextureFormat::RGBA8,
                                 static_cast<uint64_t>(flags),
                                 mem);
}

bgfx::TextureHandle BGFXAdapter::createTexture2DFromData(uint16_t width, uint16_t height,
                                                          bgfx::TextureFormat::Enum format,
                                                          const void* data, uint32_t size,
                                                          uint64_t flags)
{
    if (width == 0 || height == 0 || data == nullptr || size == 0) {
        return BGFX_INVALID_HANDLE;
    }
    const bgfx::Memory* mem = bgfx::copy(data, size);
    return bgfx::createTexture2D(width, height, false, 1, format,
                                 static_cast<uint64_t>(flags), mem);
}

void BGFXAdapter::destroy(bgfx::VertexBufferHandle h)
{
    if (bgfx::isValid(h)) {
        bgfx::destroy(h);
    }
}

void BGFXAdapter::destroy(bgfx::IndexBufferHandle h)
{
    if (bgfx::isValid(h)) {
        bgfx::destroy(h);
    }
}

void BGFXAdapter::destroy(bgfx::TextureHandle h)
{
    if (bgfx::isValid(h)) {
        bgfx::destroy(h);
    }
}

} // namespace ayt::render::detail
