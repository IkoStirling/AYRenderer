#include "detail/BGFXAdapter.h"

#include "aymath/MathTypes.h"

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
    init.callback          = nullptr;

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
    // Drain pending submits before D3D11 teardown (reduces Intel driver noise on exit).
    bgfx::frame();
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

bool BGFXAdapter::isNoopBackend() const noexcept
{
    // R5+ — true when bgfx is currently bound to its Noop backend.
    // Pass implementations (PostProcessPass today; deferred Shadow /
    // GBuffer / Lighting R5+) call this to skip GPU work that
    // wouldn't render meaningfully under Noop AND that would leak
    // bgfx resources into the shutdown path that Noop doesn't clean
    // up cleanly. Pre-initialized adapter reports false (caller
    // should also gate on isInitialized first).
    if (!_initialized) {
        return false;
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    return caps != nullptr && caps->rendererType == bgfx::RendererType::Noop;
}

bool BGFXAdapter::requestScreenshot(const std::string& filePath)
{
    if (!_initialized || filePath.empty()) {
        return false;
    }
    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, filePath.c_str());
    return true;
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

void BGFXAdapter::setViewClearNone(uint8_t viewId)
{
    bgfx::setViewClear(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);
}

void BGFXAdapter::setViewTransform(uint8_t viewId, const float* view, const float* proj)
{
    bgfx::setViewTransform(viewId, view, proj);
}

void BGFXAdapter::resetResolution(uint32_t width, uint32_t height, bool vsync)
{
    if (!_initialized) {
        return;
    }
    const uint32_t reset = vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
    bgfx::reset(width, height, reset);
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
    if (_initialized && bgfx::isValid(h)) {
        bgfx::destroy(h);
    }
}

void BGFXAdapter::destroy(bgfx::IndexBufferHandle h)
{
    if (_initialized && bgfx::isValid(h)) {
        bgfx::destroy(h);
    }
}

void BGFXAdapter::destroy(bgfx::TextureHandle h)
{
    if (_initialized && bgfx::isValid(h)) {
        bgfx::destroy(h);
    }
}

bgfx::FrameBufferHandle BGFXAdapter::createFrameBuffer(uint16_t width, uint16_t height,
                                                      bgfx::TextureFormat::Enum colorFormat,
                                                      bool withDepth)
{
    // R5+ (Phase PostProcess, 2026-07-20) — single-FBO create path.
    // Bypasses creation when the adapter isn't initialized so the
    // headless Noop-backend test path stays alive (returns invalid
    // handle → PostProcessPass::execute skips the frame's post step
    // gracefully instead of crashing on bgfx::createFrameBuffer with
    // no init).
    if (!_initialized || width == 0 || height == 0) {
        return BGFX_INVALID_HANDLE;
    }
    const uint64_t textureFlags = BGFX_TEXTURE_RT
                                | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    bgfx::TextureHandle color = bgfx::createTexture2D(
        width, height, /*hasMips=*/false, /*numLayers=*/1,
        colorFormat, textureFlags, /*mem=*/nullptr);
    if (!bgfx::isValid(color)) {
        return BGFX_INVALID_HANDLE;
    }
    bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(
        /*num=*/1, &color, /*depth=*/withDepth);
    // color is owned by the framebuffer now — bgfx manages the
    // attachment's lifetime once it's attached, so we do NOT destroy
    // `color` separately (would double-free).
    if (bgfx::isValid(fb)) {
        return fb;
    }
    return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
}

void BGFXAdapter::setViewFrameBuffer(uint8_t viewId, bgfx::FrameBufferHandle fb)
{
    if (!_initialized) {
        return;
    }
    bgfx::setViewFrameBuffer(viewId, fb);
}

void BGFXAdapter::destroy(bgfx::FrameBufferHandle h)
{
    // R5+ — destroy frees the color attachment automatically (bgfx
    // tracks the lifetime relationship). Same pattern as the VB/IB/
    // TextureHandle destroys above: no-op on invalid handle or when
    // not initialized (e.g. Renderer::shutdown ran first).
    if (_initialized && bgfx::isValid(h)) {
        bgfx::destroy(h);
    }
}

} // namespace ayt::render::detail
