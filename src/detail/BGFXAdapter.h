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

    // R5+ (Phase Shadow, 2026-07-20) — depth-only FBO for shadow
    // maps. Distinct from createFrameBuffer() (which allocates a color
    // attachment we don't need): a shadow map is a single D24S8
    // depth texture attached at slot 0. Caller treats the returned
    // FBO the same way as createFrameBuffer() — setViewFrameBuffer to
    // bind, bgfx::getTexture(fb, 0) to sample the depth, destroy(fb)
    // on resize/teardown. Returns invalid when the adapter is
    // uninitialized or the size is 0.
    bgfx::FrameBufferHandle createDepthOnlyFrameBuffer(uint16_t width, uint16_t height);

    // R5+ — bind `fb` as the draw target for `viewId`. Pass
    // bgfx::kInvalidFrameBufferHandle to revert to the default
    // backbuffer (called by PostProcessPass::execute after blitting
    // the offscreen result back, before returning).
    void setViewFrameBuffer(uint8_t viewId, bgfx::FrameBufferHandle fb);

    // R5+ (Pass-side backfill, 2026-07-20) — every method below
    // exists so the 4 RenderPass implementations
    // (ForwardOpaquePass / TransparentPass / ShadowPass /
    // PostProcessPass) can avoid ever calling a bgfx::*
    // *function* directly. The Pass files still include
    // <bgfx/bgfx.h> because they hold `bgfx::*Handle` members
    // (cached FBO/VB/IB IDs require type completeness), but
    // every bgfx interaction goes through one of these wrappers.
    //
    // `static` isValid() overloads stay here even though bgfx::isValid
    // is query-only with no side effects — pinning the rule "no
    // `bgfx::` function names in Pass files" makes grep audits
    // trivial (one regex matches BGFXAdapter.cpp + .h and nothing
    // else under src/detail/*Pass*).
    void setState(uint64_t state);
    void setTransformIdentity();
    void setViewClearRaw(uint8_t viewId, uint16_t flags,
                         uint32_t rgba = 0,
                         float    depth = 1.0f,
                         uint8_t  stencil = 0);
    void setViewClearDepthOnly(uint8_t viewId, float depth = 1.0f);

    void submit(uint8_t viewId,
                bgfx::ProgramHandle program =
                    bgfx::ProgramHandle{BGFX_INVALID_HANDLE},
                uint32_t depth = 0,
                uint8_t  flags = BGFX_DISCARD_NONE);

    static bool isValid(bgfx::VertexBufferHandle h);
    static bool isValid(bgfx::IndexBufferHandle  h);
    static bool isValid(bgfx::TextureHandle     h);
    static bool isValid(bgfx::FrameBufferHandle h);

    // R5+ (Pass-side backfill, 2026-07-20) — borrowed FBO
    // attachment accessor. Pass callers (PostProcessPass reads the
    // color attach to feed a sampler; future GBufferPass reads MRT
    // outputs) need a way to "get the TextureHandle for an FBO
    // attachment slot" without importing <bgfx/bgfx.h> just for
    // the bgfx::getTexture function. The returned handle has no
    // destroy obligation; it's owned by the FBO and dies with it.
    // Gated on isInitialized() so uninitialized adapters return
    // an invalid handle (the same shape as Noop) — keeps callers
    // safe under headless test paths.
    bgfx::TextureHandle getFboAttachment(bgfx::FrameBufferHandle fb,
                                         uint8_t attachment = 0);

    void destroy(bgfx::VertexBufferHandle h);
    void destroy(bgfx::IndexBufferHandle h);
    void destroy(bgfx::TextureHandle h);
    void destroy(bgfx::FrameBufferHandle h);

    static bgfx::RendererType::Enum mapBackend(Backend backend);

    // Process-wide: true after at least one successful initialize() in
    // this process that left bgfx alive (including sticky Noop).
    static bool isProcessBgfxAlive() noexcept;

private:
    bool _initialized = false;
};

} // namespace ayt::render::detail
