#include "detail/BGFXAdapter.h"

#include "aymath/MathTypes.h"

#include <cstdlib>
#include <mutex>

namespace ayt::render::detail
{

// Process-wide bgfx lifetime. Unit tests construct many Renderer
// instances back-to-back; calling bgfx::shutdown() between them on the
// Noop backend leaves stale handle tables and UAF on the next
// submit (Test_LightingCamera → subsequent suite SIGSEGV).
//
// Contract:
//   - First initialize() → bgfx::init, refcount = 1
//   - Nested initialize() → refcount++, no second init
//   - shutdown() → refcount--; if >0 keep bgfx
//   - Last shutdown on Noop → sticky keep-alive (no bgfx::shutdown)
//   - Last shutdown on real GPU → bgfx::shutdown (editor/demo teardown)
//   - atexit drains sticky Noop so the process exits cleanly
namespace bgfx_life {

std::mutex& mutex()
{
    static std::mutex* m = new std::mutex();
    return *m;
}

int& refCount()
{
    static int* count = new int(0);
    return *count;
}

bool& noopSticky()
{
    static bool* sticky = new bool(false);
    return *sticky;
}

bgfx::RendererType::Enum& liveType()
{
    static auto* t = new bgfx::RendererType::Enum(bgfx::RendererType::Count);
    return *t;
}

bool& ateExitRegistered()
{
    static bool* r = new bool(false);
    return *r;
}

void shutdownProcessUnlocked()
{
    if (refCount() > 0 || noopSticky()
        || liveType() != bgfx::RendererType::Count) {
        bgfx::frame();
        bgfx::shutdown();
    }
    refCount() = 0;
    noopSticky() = false;
    liveType() = bgfx::RendererType::Count;
}

void atexitHandler()
{
    std::lock_guard<std::mutex> lock(mutex());
    shutdownProcessUnlocked();
}

void registerAteExitOnce()
{
    if (ateExitRegistered()) {
        return;
    }
    ateExitRegistered() = true;
    std::atexit(&atexitHandler);
}

} // namespace bgfx_life

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

bool BGFXAdapter::isProcessBgfxAlive() noexcept
{
    std::lock_guard<std::mutex> lock(bgfx_life::mutex());
    return bgfx_life::refCount() > 0 || bgfx_life::noopSticky();
}

bool BGFXAdapter::initialize(const BGFXInitParams& params)
{
    if (_initialized) {
        return true;
    }

    std::lock_guard<std::mutex> lock(bgfx_life::mutex());

    const bgfx::RendererType::Enum requested = mapBackend(params.backend);
    const bool bgfxAlreadyUp =
        (bgfx_life::refCount() > 0) || bgfx_life::noopSticky();

    if (bgfxAlreadyUp) {
        // Reject hard backend switches while a context is sticky/live
        // (e.g. Noop tests then a real GPU request in the same process).
        if (requested != bgfx::RendererType::Count
            && bgfx_life::liveType() != bgfx::RendererType::Count
            && requested != bgfx_life::liveType()) {
            return false;
        }

        const uint32_t reset = params.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
        bgfx::reset(params.width, params.height, reset);

        ++bgfx_life::refCount();
        bgfx_life::noopSticky() = false;
        _initialized = true;
        return true;
    }

    bgfx::Init init;
    init.type     = requested;
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

    const bgfx::Caps* caps = bgfx::getCaps();
    bgfx_life::liveType() =
        (caps != nullptr) ? caps->rendererType : requested;
    bgfx_life::refCount() = 1;
    bgfx_life::noopSticky() = false;
    bgfx_life::registerAteExitOnce();

    _initialized = true;
    return true;
}

void BGFXAdapter::shutdown()
{
    if (!_initialized) {
        return;
    }

    std::lock_guard<std::mutex> lock(bgfx_life::mutex());

    // Drain pending submits before dropping our claim (or tearing down).
    bgfx::frame();
    _initialized = false;

    if (bgfx_life::refCount() > 0) {
        --bgfx_life::refCount();
    }

    if (bgfx_life::refCount() > 0) {
        return;
    }

    // Last adapter: keep Noop alive for the rest of the process so the
    // next Renderer::initialize does not re-enter bgfx::init (unsafe on
    // Noop). Real GPU backends still shut down so editor/demo exit is
    // clean for the driver.
    if (bgfx_life::liveType() == bgfx::RendererType::Noop) {
        bgfx_life::noopSticky() = true;
        return;
    }

    bgfx::shutdown();
    bgfx_life::liveType() = bgfx::RendererType::Count;
    bgfx_life::noopSticky() = false;
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
    // R5+ — true when the underlying bgfx backend is Noop. Useful for
    // Pass implementations that want to skip GPU work on the headless
    // test path without breaking their lifecycle hooks (the Noop
    // backend still returns valid handles from create* so handle
    // validity alone is not a reliable "skip me" signal). Pre-initialized
    // adapter reports false (caller should also gate on isInitialized).
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
