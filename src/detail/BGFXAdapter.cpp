#include "detail/BGFXAdapter.h"
#include "detail/BgfxMatrix.h"

#include "aymath/MathTypes.h"

#include <cstdio>
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

uint32_t bgfxResetFlags(bool vsync, uint32_t msaa) noexcept
{
    uint32_t reset = vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;
    switch (msaa) {
    case 2:  reset |= BGFX_RESET_MSAA_X2;  break;
    case 4:  reset |= BGFX_RESET_MSAA_X4;  break;
    case 8:  reset |= BGFX_RESET_MSAA_X8;  break;
    case 16: reset |= BGFX_RESET_MSAA_X16; break;
    default: break;
    }
    return reset;
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
    _msaa = params.msaa;
    _backbufferW = params.width;
    _backbufferH = params.height;
    _vsync = params.vsync;

    if (bgfxAlreadyUp) {
        // Reject hard backend switches while a context is sticky/live
        // (e.g. Noop tests then a real GPU request in the same process).
        if (requested != bgfx::RendererType::Count
            && bgfx_life::liveType() != bgfx::RendererType::Count
            && requested != bgfx_life::liveType()) {
            return false;
        }

        bgfx::reset(params.width, params.height, bgfxResetFlags(params.vsync, _msaa));

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
    init.resolution.reset  = bgfxResetFlags(params.vsync, _msaa);
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

    if (bgfx::isValid(_litShadowFallback)) {
        bgfx::destroy(_litShadowFallback);
        _litShadowFallback = BGFX_INVALID_HANDLE;
    }

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
    _gpuFrameNum = bgfx::frame();
}

uint32_t BGFXAdapter::gpuFrameCounter() const noexcept
{
    return _gpuFrameNum;
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

void BGFXAdapter::setViewTransform(uint8_t viewId,
                                   const ayt::math::Float4x4& view,
                                   const ayt::math::Float4x4& proj)
{
    float viewCol[16];
    float projCol[16];
    toBgfxColumnMajor(view, viewCol);
    toBgfxColumnMajor(proj, projCol);
    bgfx::setViewTransform(viewId, viewCol, projCol);
}

void BGFXAdapter::setViewTransformColumnMajor(uint8_t viewId,
                                              const float viewColMajor[16],
                                              const float projColMajor[16])
{
    bgfx::setViewTransform(viewId, viewColMajor, projColMajor);
}

void BGFXAdapter::resetResolution(uint32_t width, uint32_t height, bool vsync)
{
    if (!_initialized) {
        return;
    }
    _backbufferW = width;
    _backbufferH = height;
    _vsync = vsync;
    bgfx::reset(width, height, bgfxResetFlags(vsync, _msaa));
}

void BGFXAdapter::setMsaaSampleCount(uint32_t samples)
{
    uint32_t msaa = 0;
    switch (samples) {
    case 2: case 4: case 8: case 16: msaa = samples; break;
    default: msaa = 0; break;
    }
    if (!_initialized) {
        _msaa = msaa;
        return;
    }
    if (_msaa == msaa) {
        return;
    }
    _msaa = msaa;
    if (_backbufferW == 0 || _backbufferH == 0) {
        return;
    }
    bgfx::reset(_backbufferW, _backbufferH, bgfxResetFlags(_vsync, _msaa));
    std::fprintf(stderr, "[BGFXAdapter] MSAA samples=%u\n", static_cast<unsigned>(_msaa));
}

void BGFXAdapter::setTransform(const ayt::math::Float4x4& world)
{
    float colMajor[16];
    toBgfxColumnMajor(world, colMajor);
    bgfx::setTransform(colMajor);
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

bgfx::TextureHandle BGFXAdapter::createTextureCube(uint16_t size,
                                                    const void* rgba8Faces,
                                                    uint64_t flags)
{
    if (!_initialized || isNoopBackend() || size == 0 || rgba8Faces == nullptr) {
        return BGFX_INVALID_HANDLE;
    }
    // 6 faces × size × size × RGBA8
    const uint32_t bytes = static_cast<uint32_t>(size) * size * 4u * 6u;
    const bgfx::Memory* mem = bgfx::copy(rgba8Faces, bytes);
    return bgfx::createTextureCube(size, false, 1,
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
    // Third arg historically ignored depth (color-only RT). When the
    // host asks for depth, build RGBA8+D24S8 so FO depth test works —
    // color-only scene FBOs draw later meshes over earlier ones
    // (cube "behind" ground with shadow still on the floor).
    if (withDepth) {
        bgfx::destroy(color);
        return createColorDepthFrameBuffer(width, height);
    }
    bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(
        /*num=*/1, &color, /*destroyTextures=*/true);
    // color is owned by the framebuffer now — bgfx manages the
    // attachment's lifetime once it's attached, so we do NOT destroy
    // `color` separately (would double-free).
    if (bgfx::isValid(fb)) {
        return fb;
    }
    return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
}

bgfx::FrameBufferHandle BGFXAdapter::createColorDepthFrameBuffer(uint16_t width,
                                                                  uint16_t height)
{
    if (!_initialized || width == 0 || height == 0) {
        return BGFX_INVALID_HANDLE;
    }

    const uint64_t colorFlags = BGFX_TEXTURE_RT
                              | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                              | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    const uint64_t depthFlags = BGFX_TEXTURE_RT
                              | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                              | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;

    // Prefer RGBA8: R32F often plain-samples as 0 on D3D when used as a
    // color RT, which makes step() mark every fragment shadowed (dark
    // silhouettes, no contact blob). 8-bit is fine once large ground
    // planes are excluded from casting.
    bgfx::TextureFormat::Enum colorFormats[] = {
        bgfx::TextureFormat::RGBA8,
        bgfx::TextureFormat::RGBA32F,
        bgfx::TextureFormat::R32F,
    };
    bgfx::TextureFormat::Enum depthFormats[] = {
        bgfx::TextureFormat::D24S8,
        bgfx::TextureFormat::D32F,
    };

    for (bgfx::TextureFormat::Enum colorFmt : colorFormats) {
        if (!bgfx::isTextureValid(0, false, 1, colorFmt, colorFlags)) {
            continue;
        }
        bgfx::TextureHandle color = bgfx::createTexture2D(
            width, height, false, 1, colorFmt, colorFlags, nullptr);
        if (!bgfx::isValid(color)) {
            continue;
        }
        for (bgfx::TextureFormat::Enum depthFmt : depthFormats) {
            if (!bgfx::isTextureValid(0, false, 1, depthFmt, depthFlags)) {
                continue;
            }
            bgfx::TextureHandle depth = bgfx::createTexture2D(
                width, height, false, 1, depthFmt, depthFlags, nullptr);
            if (!bgfx::isValid(depth)) {
                continue;
            }
            const bgfx::TextureHandle attachments[2] = {color, depth};
            bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(
                2, attachments, /*destroyTextures=*/true);
            if (bgfx::isValid(fb)) {
                return fb;
            }
            bgfx::destroy(depth);
        }
        bgfx::destroy(color);
    }
    return BGFX_INVALID_HANDLE;
}

bgfx::FrameBufferHandle BGFXAdapter::createGbufferFrameBuffer(uint16_t width, uint16_t height)
{
    // §P5 B4a (2026-07-22) — 4-attach GBuffer MRT.
    // Mirror `createColorDepthFrameBuffer` shape (uninit guard +
    // format probe + cleanup-on-failure). depth 末位 is bgfx 约定
    // (verified at the 2-attach `createColorDepthFrameBuffer` site
    // above; same convention extends to num=4).
    //
    // B4a ships ONLY the MRT allocation path — actual draw dispatch
    // (iterate RenderScene, bind shader, submit) lands in B4c.
    // Tests (Test_B4_GBufferMRT) exercise this helper + the
    // GBufferPass::ensure plumbing, NOT any GPU draw.
    if (!_initialized || isNoopBackend() || width == 0 || height == 0) {
        return BGFX_INVALID_HANDLE;
    }

    constexpr uint8_t kNumColor = 3;
    // RT0 albedo RGBA8 / RT1 normal RGBA8 / RT2 worldPos RGBA16F.
    // RGBA8 on RT2 quantized worldPos (~0.16m) → mosaic shadow UVs.
    const bgfx::TextureFormat::Enum colorFmts[kNumColor] = {
        bgfx::TextureFormat::RGBA8,
        bgfx::TextureFormat::RGBA8,
        bgfx::TextureFormat::RGBA16F,
    };
    const uint64_t colorFlags = BGFX_TEXTURE_RT
                              | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                              | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    const uint64_t depthFlags = colorFlags;  // D24S8 same flags

    bgfx::TextureHandle colors[kNumColor] = {
        bgfx::TextureHandle{BGFX_INVALID_HANDLE},
        bgfx::TextureHandle{BGFX_INVALID_HANDLE},
        bgfx::TextureHandle{BGFX_INVALID_HANDLE},
    };
    bool ok = true;
    for (uint8_t i = 0; i < kNumColor; ++i) {
        if (!bgfx::isTextureValid(0, false, 1, colorFmts[i], colorFlags)) {
            ok = false;
            break;
        }
        colors[i] = bgfx::createTexture2D(width, height, false, 1,
                                          colorFmts[i], colorFlags, nullptr);
        if (!bgfx::isValid(colors[i])) {
            ok = false;
            break;
        }
    }

    bgfx::TextureHandle depth = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    if (ok) {
        if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D24S8, depthFlags)) {
            ok = false;
        } else {
            depth = bgfx::createTexture2D(width, height, false, 1,
                                          bgfx::TextureFormat::D24S8, depthFlags, nullptr);
            if (!bgfx::isValid(depth)) {
                ok = false;
            }
        }
    }

    if (!ok) {
        // Cleanup partial creation (strictly better than the existing
        // `createColorDepthFrameBuffer` which only destroys color on
        // failure — see BGFXAdapter.cpp:506). Mirror
        // ShadowMapResources::destroy cleanup discipline.
        for (uint8_t i = 0; i < kNumColor; ++i) {
            if (bgfx::isValid(colors[i])) {
                bgfx::destroy(colors[i]);
            }
        }
        if (bgfx::isValid(depth)) {
            bgfx::destroy(depth);
        }
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }

    // 4-attach order: [albedo, normal, worldPos, depth]
    const bgfx::TextureHandle attachments[4] = {
        colors[0], colors[1], colors[2], depth
    };
    bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(
        /*num=*/4, attachments, /*destroyTextures=*/true);

    if (!bgfx::isValid(fb)) {
        // FBO creation failed — textures are owned by bgfx when
        // destroyTextures=true was honored upstream. Caller treats
        // invalid handle as "skip GBuffer this frame".
        return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }

    return fb;
}

void BGFXAdapter::setViewFrameBuffer(uint8_t viewId, bgfx::FrameBufferHandle fb)
{
    if (!_initialized) {
        return;
    }
    bgfx::setViewFrameBuffer(viewId, fb);
}

bgfx::FrameBufferHandle BGFXAdapter::createDepthOnlyFrameBuffer(uint16_t width, uint16_t height)
{
    // R5+ (Phase Shadow) — single depth RT for the shadow map.
    // Prefer the TextureHandle* createFrameBuffer overload (same path
    // as createFrameBuffer color RT and bgfx examples/16-shadowmaps):
    // it Attachment::init's with resolve=NONE for depth. The previous
    // hand-rolled Attachment{handle, access} left mip/layer/numLayers
    // uninitialized → bgfx isFrameBufferValid ErrorAssert / debugBreak
    // on first ShadowPass::execute under a Debug build.
    if (!_initialized || width == 0 || height == 0) {
        return BGFX_INVALID_HANDLE;
    }

    const uint64_t textureFlags = BGFX_TEXTURE_RT
                                | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                                | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;

    // Try D24S8 first (matches F2 sampling of .r); fall back to D32F
    // when the backend reports the format unsupported as an RT.
    bgfx::TextureFormat::Enum formats[] = {
        bgfx::TextureFormat::D24S8,
        bgfx::TextureFormat::D32F,
    };

    for (bgfx::TextureFormat::Enum fmt : formats) {
        if (!bgfx::isTextureValid(0, false, 1, fmt, textureFlags)) {
            continue;
        }
        bgfx::TextureHandle depth = bgfx::createTexture2D(
            width, height, /*hasMips=*/false, /*numLayers=*/1,
            fmt, textureFlags, /*mem=*/nullptr);
        if (!bgfx::isValid(depth)) {
            continue;
        }
        bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(
            /*num=*/1, &depth, /*destroyTextures=*/true);
        if (bgfx::isValid(fb)) {
            return fb;
        }
        // FBO create failed — TextureHandle* overload with
        // destroyTextures=true still owns `depth` only when the FBO
        // handle is valid. On failure the texture is ours to free.
        bgfx::destroy(depth);
    }
    return bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
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

// R5+ (Pass-side backfill, 2026-07-20) — 9 thin wrappers around bgfx
// so RenderPass implementations never directly call bgfx::*. Each is
// one line today and one line tomorrow if bgfx changes its API.
// The wrappers also let us add per-Pass guardrails in future (logging,
// perf counters, leak detection) without touching every Pass.

void BGFXAdapter::setState(uint64_t state)
{
    bgfx::setState(state);
}

void BGFXAdapter::setTransformIdentity()
{
    bgfx::setTransform(nullptr);
}

// P6.5 (§6 Pass-side backfill, 2026-07-22) — preset state
// combinations. Each preset is a pure passthrough to bgfx::setState
// with the same bit combination the pre-P6.5 Pass code spelled
// inline (verified line-by-line against the inline state expressions
// at ForwardOpaquePass.cpp:184-186 / TransparentPass.cpp:26-30 /
// PostProcessPass.cpp:219-220 / ShadowPass casterDrawState).
void BGFXAdapter::setStateOpaque()
{
    bgfx::setState(BGFX_STATE_WRITE_RGB
                 | BGFX_STATE_WRITE_A
                 | BGFX_STATE_WRITE_Z
                 | BGFX_STATE_DEPTH_TEST_LESS
                 | BGFX_STATE_CULL_CW);
}

void BGFXAdapter::setStateAlphaBlend()
{
    bgfx::setState(BGFX_STATE_WRITE_RGB
                 | BGFX_STATE_WRITE_A
                 | BGFX_STATE_BLEND_ALPHA
                 | BGFX_STATE_DEPTH_TEST_LESS
                 | BGFX_STATE_CULL_CW);
}

void BGFXAdapter::setStateDepthTestAlways()
{
    bgfx::setState(BGFX_STATE_WRITE_RGB
                 | BGFX_STATE_WRITE_A
                 | BGFX_STATE_DEPTH_TEST_ALWAYS);
}

void BGFXAdapter::setStateDepthOnlyWrite()
{
    // Shadow caster depth write — same bits as
    // ShadowMapResources::casterDrawState() (PR-F1' shipped).
    bgfx::setState(BGFX_STATE_WRITE_Z
                 | BGFX_STATE_DEPTH_TEST_LESS);
}

bgfx::VertexLayout BGFXAdapter::vertexLayoutPosUv()
{
    // PostProcessPass fullscreen triangle — Position 2 floats +
    // TexCoord0 2 floats (matches the FullscreenVertex POD stride
    // and the Phoskia VS UV rebuild from pos.xy). Returning a fresh
    // layout each call is intentional: bgfx::VertexLayout is a
    // tiny POD (~32 bytes) and PostProcessPass caches it locally
    // via ensureFullscreenQuad(); no leak risk.
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    return layout;
}

void BGFXAdapter::touch(uint8_t viewId)
{
    if (!_initialized) {
        return;
    }
    bgfx::touch(viewId);
}

bool BGFXAdapter::capsHomogeneousDepth() const noexcept
{
    if (!_initialized) {
        return false;
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    return caps != nullptr && caps->homogeneousDepth;
}

bool BGFXAdapter::capsTextureBlit() const noexcept
{
    if (!_initialized) {
        return false;
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    return caps != nullptr && (caps->supported & BGFX_CAPS_TEXTURE_BLIT) != 0;
}

bool BGFXAdapter::capsTextureReadBack() const noexcept
{
    // Pre-existing supportsTextureReadBack() already does this — re-
    // exported under the caps* prefix so ShadowPass's grep audit
    // finds one consistent naming style for capability queries.
    return supportsTextureReadBack();
}

void BGFXAdapter::setViewClearRaw(uint8_t viewId, uint16_t flags,
                                  uint32_t rgba, float depth, uint8_t stencil)
{
    bgfx::setViewClear(viewId, flags, rgba, depth, stencil);
}

void BGFXAdapter::setPaletteColor(uint8_t index, float r, float g, float b, float a)
{
    if (!_initialized) {
        return;
    }
    bgfx::setPaletteColor(index, r, g, b, a);
}

void BGFXAdapter::setViewClearPalette(uint8_t viewId, uint16_t flags,
                                      uint8_t paletteIndex, float depth,
                                      uint8_t stencil)
{
    if (!_initialized) {
        return;
    }
    // Palette overload sets BGFX_CLEAR_COLOR_USE_PALETTE internally when
    // attachment indices are not UINT8_MAX (see bgfx_p.h Clear ctor).
    bgfx::setViewClear(viewId, flags, depth, stencil,
                       paletteIndex, paletteIndex, paletteIndex, paletteIndex,
                       paletteIndex, paletteIndex, paletteIndex, paletteIndex);
}

void BGFXAdapter::setViewClearDepthOnly(uint8_t viewId, float depth)
{
    // R5+ — convenience for depth-only FBOs (ShadowPass). Clears the
    // depth attachment; color RGBA / stencil left at defaults (bgfx
    // ignores them when the FBO has no color/stencil attachments).
    bgfx::setViewClear(viewId,
                       BGFX_CLEAR_DEPTH,
                       /*rgba=*/0x00000000,
                       /*depth=*/depth,
                       /*stencil=*/0);
}

void BGFXAdapter::submit(uint8_t viewId,
                          bgfx::ProgramHandle program,
                          uint32_t depth, uint8_t flags)
{
    // bgfx::submit returns void on this version (caller observes
    // the draw-call count via the bgfx stats callback, not via the
    // submit return). Passes that want draw counts must read the
    // adapter-side counter (BGFXAdapter::lastDrawCalls) when bgfx
    // exposes that — for now this is a pure proxy.
    bgfx::submit(viewId, program, depth, flags);
}

bool BGFXAdapter::isValid(bgfx::VertexBufferHandle h) { return bgfx::isValid(h); }
bool BGFXAdapter::isValid(bgfx::IndexBufferHandle  h) { return bgfx::isValid(h); }
bool BGFXAdapter::isValid(bgfx::TextureHandle     h) { return bgfx::isValid(h); }
bool BGFXAdapter::isValid(bgfx::FrameBufferHandle h) { return bgfx::isValid(h); }

bgfx::TextureHandle BGFXAdapter::getFboAttachment(bgfx::FrameBufferHandle fb,
                                                   uint8_t attachment)
{
    // R5+ (Pass-side backfill) — borrowed accessor, matching the
    // bgfx contract that the returned TextureHandle is owned by the
    // FBO and does NOT need destroy(). We gate on isInitialized so
    // uninitialized adapters return an invalid handle (callers are
    // trained to check before sampling, so the headless test path
    // degrades to "skip the sampler bind").
    if (!_initialized) {
        return BGFX_INVALID_HANDLE;
    }
    return bgfx::getTexture(fb, attachment);
}

bool BGFXAdapter::blitTexture(uint8_t viewId,
                              bgfx::TextureHandle dst,
                              bgfx::TextureHandle src,
                              uint16_t width,
                              uint16_t height)
{
    if (!_initialized || !bgfx::isValid(dst) || !bgfx::isValid(src)) {
        return false;
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    if (caps == nullptr || (caps->supported & BGFX_CAPS_TEXTURE_BLIT) == 0) {
        return false;
    }
    bgfx::blit(viewId, dst, 0, 0, src, 0, 0, width, height);
    return true;
}

bool BGFXAdapter::blitTextureRegion(uint8_t viewId,
                                      bgfx::TextureHandle dst,
                                      uint16_t dstX,
                                      uint16_t dstY,
                                      bgfx::TextureHandle src,
                                      uint16_t srcX,
                                      uint16_t srcY,
                                      uint16_t width,
                                      uint16_t height)
{
    if (!_initialized || !bgfx::isValid(dst) || !bgfx::isValid(src)) {
        return false;
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    if (caps == nullptr || (caps->supported & BGFX_CAPS_TEXTURE_BLIT) == 0) {
        return false;
    }
    bgfx::blit(viewId, dst, dstX, dstY, src, srcX, srcY, width, height);
    return true;
}

bgfx::TextureHandle BGFXAdapter::createBlitDstTexture2D(uint16_t width, uint16_t height)
{
    if (!_initialized || width == 0 || height == 0) {
        return BGFX_INVALID_HANDLE;
    }
    const uint64_t flags = BGFX_TEXTURE_BLIT_DST
                         | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                         | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA8, flags)) {
        return BGFX_INVALID_HANDLE;
    }
    return bgfx::createTexture2D(width, height, false, 1,
                                 bgfx::TextureFormat::RGBA8, flags, nullptr);
}

bool BGFXAdapter::supportsTextureReadBack() const noexcept
{
    if (!_initialized) {
        return false;
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    return caps != nullptr && (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0;
}

bool BGFXAdapter::requestTextureReadback(bgfx::TextureHandle tex,
                                         void* rgba8Out,
                                         uint32_t& outReadyFrame) const
{
    if (!_initialized || rgba8Out == nullptr || !supportsTextureReadBack()
        || !bgfx::isValid(tex)) {
        return false;
    }
    outReadyFrame = bgfx::readTexture(tex, rgba8Out, 0);
    return true;
}

bgfx::TextureHandle BGFXAdapter::getLitShadowFallbackTexture()
{
    if (!_initialized) {
        return BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(_litShadowFallback)) {
        return _litShadowFallback;
    }
    // R=1 so step(refDepth - bias, occluder) → 1 (fully lit) for any
    // reasonable refDepth in [0,1].
    const uint8_t pixel[4] = {255, 255, 255, 255};
    const bgfx::Memory* mem = bgfx::copy(pixel, sizeof(pixel));
    _litShadowFallback = bgfx::createTexture2D(
        1, 1, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        mem);
    return _litShadowFallback;
}

} // namespace ayt::render::detail
