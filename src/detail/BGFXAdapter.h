#pragma once

#include "AYRenderer/RenderTypes.h"

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
    // Backbuffer MSAA (0/2/4/8/16). Softens mesh silhouettes; 4 is a
    // good default for Editor Game View without a large cost.
    uint32_t msaa = 4;
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

    uint32_t gpuFrameCounter() const noexcept;

    bool requestScreenshot(const std::string& filePath);

    void setViewRect(uint8_t viewId, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    // §P5.5 C (2026-07-23) — per-light shadow atlas sub-rect clip.
    // bgfx::setViewScissor wraps the fragment-cull rect (orthogonal
    // to setViewRect which is the viewport). ShadowPass calls this
    // per-caster-slot inside its multi-light scissor loop. Noop-safe
    // (returns early when !_initialized).
    void setViewScissor(uint8_t viewId, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h);
    void setViewClear(uint8_t viewId, const ClearDesc& clear);
    void setViewClearNone(uint8_t viewId);
    // View/proj must be true AYMath row-major Float4x4; converted to
    // bgfx column-major at the call boundary (see detail/BgfxMatrix.h).
    void setViewTransform(uint8_t viewId,
                          const ayt::math::Float4x4& view,
                          const ayt::math::Float4x4& proj);
    // bx/bgfx column-major view/proj — use for shadow caster MVP so it
    // matches u_lightViewProj without AYMath round-trip drift.
    void setViewTransformColumnMajor(uint8_t viewId,
                                     const float viewColMajor[16],
                                     const float projColMajor[16]);

    void resetResolution(uint32_t width, uint32_t height, bool vsync);
    void setMsaaSampleCount(uint32_t samples);
    uint32_t msaaSampleCount() const noexcept { return _msaa; }

    // World matrix: true AYMath → column-major for bgfx::setTransform.
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
    // Empty mutable RGBA8 texture (mem=nullptr). Required for
    // updateTexture2D / video frame upload (AYVideo V3 §12).
    bgfx::TextureHandle createDynamicTexture2D(
        uint16_t width, uint16_t height,
        uint64_t flags = BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);
    // CPU → GPU full-rect update. `rgba8Data` must be width*height*4 bytes.
    void updateTexture2D(bgfx::TextureHandle handle,
                         uint16_t width, uint16_t height,
                         const void* rgba8Data);
    // §P5.5 D-upload — RGBA8 cube. `rgba8Faces` = 6 contiguous faces
    // (+X,-X,+Y,-Y,+Z,-Z), each size×size×4 bytes.
    bgfx::TextureHandle createTextureCube(uint16_t size,
                                          const void* rgba8Faces,
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

    // R5+ (Phase Shadow, 2026-07-20) — depth-only FBO (legacy / tests).
    // Prefer createColorDepthFrameBuffer for sampleable shadow maps:
    // D3D11 plain-samples of raw depth textures return 0.
    bgfx::FrameBufferHandle createDepthOnlyFrameBuffer(uint16_t width, uint16_t height);

    // Shadow map: float color (encoded depth in .r) + D24S8/D32F depth
    // for caster z-test. Prefers R32F (RGBA8 is only 8-bit and causes
    // severe self-shadow acne → nearly-black lit meshes). Attachment 0
    // is the sampleable color RT.
    // Also used for the Editor/scene color+depth RT (PostProcess sample).
    bgfx::FrameBufferHandle createColorDepthFrameBuffer(uint16_t width, uint16_t height);

    // §P5 B5.5 (2026-07-23) — GBuffer MRT helper. 4-attach:
    // RT0 albedo RGBA8 / RT1 normal RGBA8 / RT2 worldPos RGBA16F
    // / RT3 depth D24S8 hardware. Adapter wraps `bgfx::createFrameBuffer`
    // num=4 form (bgfx.h:3393-3397) so Pass files never see raw bgfx
    // (cutsheet §6 red line — passes go through adapter only).
    //
    // bgfx 约定 depth attachment 末位(bgfx docs + verified at
    // `createColorDepthFrameBuffer` 2-attach pattern in this header).
    // `destroyTextures=true` 让 FBO 拥有 attachments,匹配
    // `ShadowMapResources::destroy` 的"勿 double-free"约定。
    //
    // Noop backend → BGFX_INVALID_HANDLE(cutsheet §1.7 adapter Noop gate,
    // 镜像 `createColorDepthFrameBuffer` 的 `_initialized` 闸)。
    //
    // Failure modes: returns BGFX_INVALID_HANDLE. Caller treats that as
    // "no GBuffer RT this frame → consumers gate on isReady()" cutsheet
    // §1.7 模式。
    //
    // Reused by: detail/GBufferPass::ensure (B4a Plumbing)。
    // Public API: NO ── adapter header is src/detail/ (not in include/),
    // 不算 cutsheet §8 public-API 扩面。
    bgfx::FrameBufferHandle createGbufferFrameBuffer(uint16_t width, uint16_t height);

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

    // P6.5 (§6 Pass-side backfill, 2026-07-22) — preset state
    // combinations. These exist so the 4 Pass implementations can drop
    // their direct use of `BGFX_STATE_*` macros and call one of these
    // named presets instead. The state bit combinations here match the
    // pre-P6.5 inline values byte-for-byte; this is a pure refactor.
    //
    // Presets:
    //   setStateOpaque           — ForwardOpaquePass default draw.
    //   setStateAlphaBlend       — TransparentPass (BLEND_ALPHA,
    //                                no WRITE_Z to let the opaque
    //                                z-buffer act as the depth test).
    //   setStateDepthTestAlways  — PostProcessPass fullscreen blit
    //                                (always-passes so the blit is
    //                                never depth-tested against the
    //                                scene FBO's leftover depth).
    //   setStateDepthOnlyWrite   — ShadowPass caster depth write.
    void setStateOpaque();
    void setStateAlphaBlend();
    void setStateDepthTestAlways();
    void setStateDepthOnlyWrite();
    // Selection inverted-hull: cull front faces (CCW), depth LESS,
    // no blend — rim stays outside the already-drawn mesh.
    void setStateOutlineHull();

    // Borrow existing color + depth textures into a transient FBO
    // (destroyTextures=false). Caller must destroy() the returned
    // handle; textures stay owned by their original FBOs.
    bgfx::FrameBufferHandle createBorrowedColorDepthFrameBuffer(
        bgfx::TextureHandle color,
        bgfx::TextureHandle depth);
    // Swap-chain color + borrowed scene depth. Do NOT use for direct
    // outline draws: a depth-only attachment FBO has zero RTVs on D3D11
    // (OMSetRenderTargets(0, NULL, dsv) discards color). EditorOverlayPass
    // renders to an RGBA8 RT + borrowed depth, then alpha-composites
    // onto the backbuffer after PostProcess.
    bgfx::FrameBufferHandle createBackbufferWithBorrowedDepthFrameBuffer(
        bgfx::TextureHandle depth);
    // P6.5 — bgfx::VertexLayout preset for the PostProcessPass
    // fullscreen triangle (Position 2 floats + TexCoord0 2 floats).
    // The returned layout is a freshly-built bgfx::VertexLayout each
    // call; PostProcessPass caches it locally. Stride must match the
    // FullscreenVertex POD (16 bytes alignas(16)).
    bgfx::VertexLayout vertexLayoutPosUv();

    // P6.5 — bgfx::touch(viewId) wrapper. ShadowPass calls this to
    // ensure the depth-only FBO view is included in bgfx's frame
    // graph even when the caster draw loop produced zero draws (e.g.
    // empty scene); without it bgfx may skip the view's clear and
    // the next frame's reader sees stale depth.
    void touch(uint8_t viewId);

    // P6.5 — capability query wrappers so ShadowPass can stop calling
    // bgfx::getCaps() directly. Return false when the adapter is not
    // initialized (matches the Noop-backend "no caps" contract).
    bool capsHomogeneousDepth() const noexcept;
    bool capsTextureBlit() const noexcept;
    bool capsTextureReadBack() const noexcept;

    void setViewClearRaw(uint8_t viewId, uint16_t flags,
                         uint32_t rgba = 0,
                         float    depth = 1.0f,
                         uint8_t  stencil = 0);
    // Float RT clear via palette (R32F shadow color needs true 1.0f far).
    void setPaletteColor(uint8_t index, float r, float g, float b, float a);
    void setViewClearPalette(uint8_t viewId, uint16_t flags,
                             uint8_t paletteIndex, float depth = 1.0f,
                             uint8_t stencil = 0);
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

    // Copy `_src` into `_dst` (must be created with BGFX_TEXTURE_BLIT_DST).
    // Returns false when caps lack TEXTURE_BLIT or handles invalid.
    bool blitTexture(uint8_t viewId,
                     bgfx::TextureHandle dst,
                     bgfx::TextureHandle src,
                     uint16_t width,
                     uint16_t height);

    bool blitTextureRegion(uint8_t viewId,
                           bgfx::TextureHandle dst,
                           uint16_t dstX,
                           uint16_t dstY,
                           bgfx::TextureHandle src,
                           uint16_t srcX,
                           uint16_t srcY,
                           uint16_t width,
                           uint16_t height);

    // Sample-only 2D target for shadow resolve (no BGFX_TEXTURE_RT).
    bgfx::TextureHandle createBlitDstTexture2D(uint16_t width, uint16_t height);

    bool supportsTextureReadBack() const noexcept;

    // Async CPU readback (RGBA8, 4 bytes). outReadyFrame is the bgfx frame
    // index when `rgba8Out` becomes valid (typically current+2).
    bool requestTextureReadback(bgfx::TextureHandle tex,
                                void* rgba8Out,
                                uint32_t& outReadyFrame) const;

    // Shared 1x1 opaque-white RGBA8 fallback. Besides keeping an absent
    // shadow producer fully lit, this is the neutral base-color texture for
    // imported materials that only provide scalar/color factors.
    bgfx::TextureHandle getWhiteFallbackTexture();

    // Compatibility name retained for the shadow receiver path.
    bgfx::TextureHandle getLitShadowFallbackTexture();

    void destroy(bgfx::VertexBufferHandle h);
    void destroy(bgfx::IndexBufferHandle h);
    void destroy(bgfx::TextureHandle h);
    void destroy(bgfx::FrameBufferHandle h);

    static bgfx::RendererType::Enum mapBackend(Backend backend);

    // Process-wide: true after at least one successful initialize() in
    // this process that left bgfx alive (including sticky Noop).
    static bool isProcessBgfxAlive() noexcept;

private:
    bool                _initialized = false;
    uint32_t            _gpuFrameNum = 0;
    uint32_t            _msaa        = 4;
    uint32_t            _backbufferW = 0;
    uint32_t            _backbufferH = 0;
    bool                _vsync       = true;
    bgfx::TextureHandle _litShadowFallback = BGFX_INVALID_HANDLE;
};

} // namespace ayt::render::detail
