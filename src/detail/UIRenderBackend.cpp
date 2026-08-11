#include "AYUIRenderBackend.h"

#include "AYRenderer.h"
#include "detail/BgfxFontAtlas.h"
#include "detail/BGFXAdapter.h"
#include "detail/UiGpuContext.h"
#include "AYShaderResourcePool.h"

#include <AYCoreUtility.h>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace ayt::render {

namespace {

struct UiVertex {
    float    x;
    float    y;
    float    z;
    uint32_t abgr;
    float    u;
    float    v;
};

// P0 unified batch: every draw entry becomes one UiItem appended to
// frame.items in submission order (array order == z-order; runs are
// grouped at flush time, never re-sorted). Sdf items arrive in P2.
enum class UiItemKind : uint8_t { Flat, Sdf };

struct UiItem {
    UiItemKind kind        = UiItemKind::Flat;
    uint16_t   textureIdx  = UINT16_MAX;  // Flat: white / font atlas / UI texture
    uint64_t   state       = 0;           // bgfx blend state (P1 encodes BlendMode)
    float      minX        = 0.0f;
    float      minY        = 0.0f;
    float      maxX        = 0.0f;
    float      maxY        = 0.0f;
    uint32_t   abgr[4]     = {0, 0, 0, 0};  // per-corner colors: TL TR BR BL
    float      u0          = 0.0f;          // Flat only
    float      v0          = 0.0f;
    float      u1          = 1.0f;
    float      v1          = 1.0f;
    detail::UiGpuContext::SdfParams sdf;    // P2: kind==Sdf only; quad bounds
};                                          //     (minX..maxY) already expanded

uint32_t toAbgr(const ayt::math::FVector4& color)
{
    const uint8_t r = static_cast<uint8_t>(color.x * 255.0f);
    const uint8_t g = static_cast<uint8_t>(color.y * 255.0f);
    const uint8_t b = static_cast<uint8_t>(color.z * 255.0f);
    const uint8_t a = static_cast<uint8_t>(color.w * 255.0f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

float toNdcX(float px, float width)
{
    return (px / width) * 2.0f - 1.0f;
}

float toNdcY(float py, float height)
{
    return 1.0f - (py / height) * 2.0f;
}

ayt::math::FRectangle intersectRect(const ayt::math::FRectangle& a,
                                    const ayt::math::FRectangle& b)
{
    return ayt::math::FRectangle(
        std::max(a.minX, b.minX),
        std::max(a.minY, b.minY),
        std::min(a.maxX, b.maxX),
        std::min(a.maxY, b.maxY));
}

bool rectNonEmpty(const ayt::math::FRectangle& r)
{
    return r.maxX > r.minX && r.maxY > r.minY;
}

// P1: BlendMode -> bgfx blend state. Write bits are folded in here so
// every recorded item state is self-sufficient (submit() skips setState
// when the passed state is 0, so state must never be 0). Additive uses
// FUNC(SRC_ALPHA, ONE) to match the interface contract SRC*SRC_ALPHA+DST*1
// (plain BGFX_STATE_BLEND_ADD would be ONE,ONE = SRC*1+DST*1).
uint64_t blendStateBits(ayt::ui::BlendMode mode)
{
    uint64_t bits = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
    switch (mode) {
    case ayt::ui::BlendMode::Additive:
        bits |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
        break;
    case ayt::ui::BlendMode::Multiply:
        bits |= BGFX_STATE_BLEND_MULTIPLY;
        break;
    case ayt::ui::BlendMode::Screen:
        bits |= BGFX_STATE_BLEND_SCREEN;
        break;
    case ayt::ui::BlendMode::Normal:
    default:
        bits |= BGFX_STATE_BLEND_ALPHA;
        break;
    }
    return bits;
}

// P3: one live UI texture in the registry. textureIdx is the bgfx handle
// (uploaded via uploadUiTexture — linear + clamp), width/height back the
// 9-patch UV math (padding is in texture pixels, so corners map 1:1 to
// screen pixels). Handles are fake pointers from an increasing counter —
// never reused, so a stale handle can never alias a new texture.
struct TextureRef {
    uint16_t textureIdx = detail::UiGpuContext::kInvalidIdx;
    uint32_t refCount   = 0;
    uint16_t width      = 0;
    uint16_t height     = 0;
};

// Pre-P3 gray-block fallback for unknown / released handles.
const ayt::math::FVector4 kUnknownTextureColor(0.25f, 0.25f, 0.28f, 1.0f);

} // namespace

struct UIRenderBackend::FrameState {
    std::vector<UiItem>                items;              // ordered draw list (z-order)
    std::vector<UiVertex>              scratchVertices;
    std::vector<uint32_t>              scratchIndices;
    ayt::font::IFont*                  textSyncFont = nullptr;
    std::vector<ayt::math::FRectangle> clipStack;
    ayt::ui::BlendMode                 currentBlend = ayt::ui::BlendMode::Normal;
    // P3: texture registry — persistent across frames (beginFrame does not
    // touch it; handles stay valid until releaseUiTexture frees them).
    std::unordered_map<void*, TextureRef> textures;
    uintptr_t                             nextHandle = 1;  // fake-pointer counter
};

namespace {

// Takes the registry map (not FrameState — that type is private to
// UIRenderBackend) so free functions can look up handles.
const TextureRef* findTextureRef(const std::unordered_map<void*, TextureRef>& textures,
                                 void* handle)
{
    if (handle == nullptr) {
        return nullptr;
    }
    auto it = textures.find(handle);
    return (it == textures.end()) ? nullptr : &it->second;
}

} // namespace

UIRenderBackend::UIRenderBackend()
    : _frame(std::make_unique<FrameState>())
{
}

UIRenderBackend::~UIRenderBackend()
{
    shutdown();
}

bool UIRenderBackend::initialize(Renderer& renderer)
{
    detail::BGFXAdapter* adapter = renderer.bgfxAdapter();
    shader::ShaderResourcePool* pool = renderer.shaderPool();
    if (adapter == nullptr || pool == nullptr) {
        return false;
    }
    return initializeFromRenderer(renderer, *adapter, *pool);
}

bool UIRenderBackend::initializeFromRenderer(Renderer& renderer, detail::BGFXAdapter& adapter,
                                             shader::ShaderResourcePool& shaderPool)
{
    AYUNREFERENCED_PARAM(renderer);
    if (_initialized) {
        return true;
    }

    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    _gpu        = std::make_unique<detail::UiGpuContext>();
    _adapter    = &adapter;
    _shaderPool = &shaderPool;
    if (!_gpu->initialize(shaderPool, adapter)) {
        shutdownFromRenderer(adapter, shaderPool);
        return false;
    }

    _fontAtlas = std::make_unique<detail::BgfxFontAtlas>();
    if (!_fontAtlas->initialize(adapter)) {
        std::fprintf(stderr, "[UIRenderBackend] BgfxFontAtlas initialize failed\n");
        shutdownFromRenderer(adapter, shaderPool);
        return false;
    }

    _initialized = true;
    return true;
}

void UIRenderBackend::shutdown()
{
    if (!_initialized) {
        return;
    }
    if (_adapter != nullptr && _shaderPool != nullptr) {
        shutdownFromRenderer(*_adapter, *_shaderPool);
    } else {
        shutdownFromRendererWithoutAdapter();
    }
}

void UIRenderBackend::shutdownFromRenderer(detail::BGFXAdapter& adapter,
                                           shader::ShaderResourcePool& shaderPool)
{
    if (_frame != nullptr) {
        // P3: release every live UI texture before the GPU context dies.
        // nextHandle intentionally NOT reset — fake pointers never reused,
        // so handles handed out before shutdown stay dead after reinit.
        if (_gpu != nullptr) {
            for (const auto& entry : _frame->textures) {
                _gpu->releaseTextTexture(adapter, entry.second.textureIdx);
            }
        }
        _frame->textures.clear();
        _frame->items.clear();
        _frame->scratchVertices.clear();
        _frame->scratchIndices.clear();
        _frame->textSyncFont = nullptr;
    }

    if (_fontAtlas != nullptr) {
        _fontAtlas->shutdown(adapter);
        _fontAtlas.reset();
    }

    if (_gpu != nullptr) {
        _gpu->shutdown(shaderPool, adapter);
        _gpu.reset();
    }

    _adapter     = nullptr;
    _shaderPool  = nullptr;
    _initialized = false;
}

void UIRenderBackend::shutdownFromRendererWithoutAdapter()
{
    if (_frame != nullptr) {
        // No adapter to destroy through; the bgfx context is gone with the
        // renderer anyway. Drop the registry without touching GPU objects.
        _frame->textures.clear();
        _frame->items.clear();
        _frame->scratchVertices.clear();
        _frame->scratchIndices.clear();
        _frame->textSyncFont = nullptr;
    }

    _gpu.reset();
    _fontAtlas.reset();
    _adapter     = nullptr;
    _shaderPool  = nullptr;
    _initialized = false;
}

void UIRenderBackend::setFramebufferSize(uint16_t width, uint16_t height)
{
    _width  = width;
    _height = height;
}

void UIRenderBackend::beginFrame()
{
    _drawCalls = 0;

    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    FrameState& frame = *_frame;
    frame.items.clear();
    frame.clipStack.clear();
    frame.textSyncFont = nullptr;
    frame.currentBlend = ayt::ui::BlendMode::Normal;  // P1: per-frame reset

    if (frame.scratchVertices.capacity() < 1024u) {
        frame.scratchVertices.reserve(1024u);
        frame.scratchIndices.reserve(1536u);
    }

    if (_gpu == nullptr || _width < 1 || _height < 1) {
        return;
    }

    // UI chrome view — fixed high slot (255) so Final PP / bloom can
    // grow without reshuffling menus; must stay > PostProcess (13).
    _gpu->beginView(kViewId, _width, _height);
}

void UIRenderBackend::beginCanvas(const ayt::math::FRectangle& viewport)
{
    AYUNREFERENCED_PARAM(viewport);
}

void UIRenderBackend::endCanvas() {}

ayt::math::FRectangle UIRenderBackend::activeClipBounds() const
{
    if (_frame == nullptr || _frame->clipStack.empty()) {
        return ayt::math::FRectangle(0.0f, 0.0f,
                                     static_cast<float>(_width),
                                     static_cast<float>(_height));
    }
    return _frame->clipStack.back();
}

bool UIRenderBackend::clipRect(ayt::math::FRectangle& inout) const
{
    if (_frame == nullptr || _frame->clipStack.empty()) {
        return rectNonEmpty(inout);
    }
    inout = intersectRect(inout, _frame->clipStack.back());
    return rectNonEmpty(inout);
}

void UIRenderBackend::pushClip(const ayt::math::FRectangle& bounds)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    // P0: no forced flush — items keep array order (z-order), clip is
    // applied at item-record time via clipRect() CPU intersection.

    ayt::math::FRectangle clipped = bounds;
    if (!_frame->clipStack.empty()) {
        clipped = intersectRect(bounds, _frame->clipStack.back());
    }
    _frame->clipStack.push_back(clipped);
}

void UIRenderBackend::popClip()
{
    if (_frame == nullptr || _frame->clipStack.empty()) {
        return;
    }
    _frame->clipStack.pop_back();
}

void UIRenderBackend::endFrame()
{
    flushColoredRects();
    flushPendingText();
}

void UIRenderBackend::flushBatches()
{
    // P0: unified batch — flush submits everything recorded so far.
    flushColoredRects();
}

void UIRenderBackend::syncTextAtlasIfNeeded()
{
    if (_fontAtlas == nullptr || !_fontAtlas->isAtlasDirty() || _frame == nullptr) {
        return;
    }

    ayt::font::IFont* font = _frame->textSyncFont;
    if (font == nullptr) {
        font = _fontAtlas->acquireFont(14);
    }
    if (font == nullptr) {
        return;
    }

    _fontAtlas->syncAtlasToGpu(font);
}

void UIRenderBackend::drawRect(const ayt::math::FRectangle& bounds, const ayt::math::FVector4& color)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // P0 unified batch: append item; z-order is preserved by array order
    // (no mid-frame flushes — the old "cut the text batch" flush here
    // existed only to keep draw order across two separate batches).
    UiItem item;
    item.textureIdx = (_gpu != nullptr) ? _gpu->whiteTextureIdx()
                                        : detail::UiGpuContext::kInvalidIdx;
    item.state = blendStateBits(_frame->currentBlend);  // P1
    item.minX = clipped.minX;
    item.minY = clipped.minY;
    item.maxX = clipped.maxX;
    item.maxY = clipped.maxY;
    const uint32_t abgr = toAbgr(color);
    item.abgr[0] = abgr;
    item.abgr[1] = abgr;
    item.abgr[2] = abgr;
    item.abgr[3] = abgr;
    _frame->items.push_back(item);
}

void UIRenderBackend::setBlendMode(ayt::ui::BlendMode mode)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    // Recorded into every item's state at draw time; a mode change starts
    // a new flush run. Reset to Normal at beginFrame.
    _frame->currentBlend = mode;
}

void UIRenderBackend::drawGradientRect(const ayt::math::FRectangle& bounds,
                                       const ayt::math::FVector4& topColor,
                                       const ayt::math::FVector4& bottomColor)
{
    // 2-color vertical gradient — routes to the 4-color path
    // (topColor = both top corners, bottomColor = both bottom
    // corners), matching AYUI MockRenderer semantics.
    drawGradientRect(bounds, topColor, topColor, bottomColor, bottomColor);
}

void UIRenderBackend::drawGradientRect(const ayt::math::FRectangle& bounds,
                                       const ayt::math::FVector4& topLeft,
                                       const ayt::math::FVector4& topRight,
                                       const ayt::math::FVector4& bottomLeft,
                                       const ayt::math::FVector4& bottomRight)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // P1: a Flat item with per-corner colors — the existing shader
    // interpolates v_color0 across the quad, so gradients are free.
    // Corner order matches flushFlatRun: TL TR BR BL.
    UiItem item;
    item.textureIdx = (_gpu != nullptr) ? _gpu->whiteTextureIdx()
                                        : detail::UiGpuContext::kInvalidIdx;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX = clipped.minX;
    item.minY = clipped.minY;
    item.maxX = clipped.maxX;
    item.maxY = clipped.maxY;
    item.abgr[0] = toAbgr(topLeft);
    item.abgr[1] = toAbgr(topRight);
    item.abgr[2] = toAbgr(bottomRight);
    item.abgr[3] = toAbgr(bottomLeft);
    _frame->items.push_back(item);
}

void UIRenderBackend::drawBorderRect(const ayt::math::FRectangle& bounds,
                                     const ayt::math::FVector4& color,
                                     float borderWidth, float cornerRadius)
{
    // P2: single SDF item — the shader renders the ring natively with
    // per-pixel AA (the P1 body's 8-rect decomposition made square corners).
    if (borderWidth <= 0.0f) {
        return;
    }

    const float width  = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    const float w = std::min(borderWidth, std::min(width, height) * 0.5f);
    if (w <= 0.0f) {
        return;
    }

    // Degenerate: border as wide as the rect is a solid fill (same
    // fallback the interface default used).
    if (width <= w * 2.0f || height <= w * 2.0f) {
        drawRect(bounds, color);
        return;
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // Radius clamped so the ring stays inside the quad even at the corners.
    const float maxRadius = std::min(width, height) * 0.5f;
    const float r         = std::max(0.0f, std::min(cornerRadius, maxRadius - w));

    // Outside stroke spans d in (0, w) across the edge (inset -w/2) — the
    // quad must extend w past the clip rect, then be clipped again so the
    // ring cannot paint outside the clip (AA falloff clipped at the edge,
    // same as the Flat path).
    ayt::math::FRectangle outer(clipped.minX - w, clipped.minY - w,
                                clipped.maxX + w, clipped.maxY + w);
    if (!clipRect(outer)) {
        return;
    }

    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = outer.minX;
    item.minY  = outer.minY;
    item.maxX  = outer.maxX;
    item.maxY  = outer.maxY;
    item.sdf.rect        = ayt::math::FVector4(outer.minX, outer.minY, outer.maxX, outer.maxY);
    item.sdf.radius      = ayt::math::FVector4(r, r, r, r);
    item.sdf.strokeColor = color;
    item.sdf.strokeWidth = w;
    item.sdf.strokeInset = -w * 0.5f;  // Outside: ring centered on the edge
    _frame->items.push_back(item);
}

void UIRenderBackend::drawRectShadow(const ayt::math::FRectangle& bounds,
                                     const ayt::ui::IRenderBackend::ShadowStyle& shadow)
{
    // P2: single SDF item; blur is approximated by expanding the SDF radius
    // in the shader (u_radius + blur), which fades the edge over blur
    // pixels. The quad grows by |offset| + blur so the falloff is not
    // clipped at the rect edge. Transparent shadow color → nothing to draw.
    if (shadow.color.w <= 0.0f) {
        return;
    }

    const float width  = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    const float blur = std::max(0.0f, shadow.blurRadius);
    const float extX = std::fabs(shadow.offset.x) + blur;
    const float extY = std::fabs(shadow.offset.y) + blur;

    ayt::math::FRectangle outer(clipped.minX - extX, clipped.minY - extY,
                                clipped.maxX + extX, clipped.maxY + extY);
    if (!clipRect(outer)) {
        return;
    }

    const float maxRadius = std::min(width, height) * 0.5f;
    const float r         = std::max(0.0f, std::min(shadow.cornerRadius, maxRadius));

    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = outer.minX;
    item.minY  = outer.minY;
    item.maxX  = outer.maxX;
    item.maxY  = outer.maxY;
    item.sdf.rect         = ayt::math::FVector4(outer.minX, outer.minY, outer.maxX, outer.maxY);
    item.sdf.radius       = ayt::math::FVector4(r, r, r, r);
    item.sdf.shadowColor  = shadow.color;
    item.sdf.shadowOffset = shadow.offset;
    item.sdf.shadowBlur   = blur;
    _frame->items.push_back(item);
}

void* UIRenderBackend::createUiTexture(uint16_t width, uint16_t height, const void* bgraPixels)
{
    if (!_initialized || _gpu == nullptr || _adapter == nullptr || bgraPixels == nullptr
        || width == 0 || height == 0) {
        return nullptr;
    }

    const uint32_t byteSize = static_cast<uint32_t>(static_cast<uint64_t>(width) * height * 4u);
    const uint16_t idx = _gpu->uploadUiTexture(*_adapter, width, height, bgraPixels, byteSize);
    if (idx == detail::UiGpuContext::kInvalidIdx) {
        return nullptr;
    }

    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    void* handle = reinterpret_cast<void*>(_frame->nextHandle++);
    _frame->textures[handle] = TextureRef{idx, 1u, width, height};
    return handle;
}

void UIRenderBackend::releaseUiTexture(void* textureHandle)
{
    if (_frame == nullptr) {
        return;
    }
    auto it = _frame->textures.find(textureHandle);
    if (it == _frame->textures.end()) {
        return;  // unknown / double release — safe no-op
    }
    if (--it->second.refCount == 0) {
        if (_initialized && _gpu != nullptr && _adapter != nullptr) {
            _gpu->releaseTextTexture(*_adapter, it->second.textureIdx);
        }
        _frame->textures.erase(it);
    }
}

void UIRenderBackend::drawRect(const ayt::math::FRectangle& bounds, void* textureHandle,
                               const ayt::math::FRectangle& uv)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    const TextureRef* ref = findTextureRef(_frame->textures, textureHandle);
    if (ref == nullptr) {
        // Unknown / released handle — pre-P3 gray-block fallback.
        drawRect(bounds, kUnknownTextureColor);
        return;
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // Flat item on the UI texture with an opaque white tint — the shader
    // multiplies v_color0 by the texture sample, so alpha rides through.
    UiItem item;
    item.textureIdx = ref->textureIdx;
    item.state      = blendStateBits(_frame->currentBlend);
    item.minX       = clipped.minX;
    item.minY       = clipped.minY;
    item.maxX       = clipped.maxX;
    item.maxY       = clipped.maxY;
    const uint32_t abgr = 0xFFFFFFFFu;
    item.abgr[0] = abgr;
    item.abgr[1] = abgr;
    item.abgr[2] = abgr;
    item.abgr[3] = abgr;
    item.u0 = uv.minX;
    item.v0 = uv.minY;
    item.u1 = uv.maxX;
    item.v1 = uv.maxY;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawWithAlpha(const ayt::math::FRectangle& bounds, void* textureHandle,
                                    float alpha)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    const TextureRef* ref = findTextureRef(_frame->textures, textureHandle);
    if (ref == nullptr) {
        // Unknown handle — gray fallback tinted by the requested alpha.
        drawRect(bounds, ayt::math::FVector4(
                             kUnknownTextureColor.x, kUnknownTextureColor.y,
                             kUnknownTextureColor.z,
                             std::max(0.0f, std::min(1.0f, alpha))));
        return;
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // Interface contract: alpha multiplies the texture's alpha channel —
    // carried by the per-vertex color (shader does v_color0 * sample).
    UiItem item;
    item.textureIdx = ref->textureIdx;
    item.state      = blendStateBits(_frame->currentBlend);
    item.minX       = clipped.minX;
    item.minY       = clipped.minY;
    item.maxX       = clipped.maxX;
    item.maxY       = clipped.maxY;
    const uint32_t abgr = toAbgr(ayt::math::FVector4(1.0f, 1.0f, 1.0f,
                                                     std::max(0.0f, std::min(1.0f, alpha))));
    item.abgr[0] = abgr;
    item.abgr[1] = abgr;
    item.abgr[2] = abgr;
    item.abgr[3] = abgr;
    item.u0 = 0.0f;
    item.v0 = 0.0f;
    item.u1 = 1.0f;
    item.v1 = 1.0f;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawNinePatch(const ayt::math::FRectangle& bounds, void* textureHandle,
                                    const ayt::math::FRectangle& uvRegion,
                                    const ayt::math::FVector4& padding)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    const TextureRef* ref = findTextureRef(_frame->textures, textureHandle);
    if (ref == nullptr) {
        drawRect(bounds, kUnknownTextureColor);
        return;
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    const float bW = clipped.maxX - clipped.minX;
    const float bH = clipped.maxY - clipped.minY;
    if (bW <= 0.0f || bH <= 0.0f) {
        return;
    }

    // padding = (left, top, right, bottom) in TEXTURE pixels, so corners
    // keep their natural size on screen (1:1 texel mapping). Bounds smaller
    // than the corner block: scale padding down to fit so the center slice
    // never collapses; UV padding follows the same scale.
    const float padL = std::max(0.0f, padding.x);
    const float padT = std::max(0.0f, padding.y);
    const float padR = std::max(0.0f, padding.z);
    const float padB = std::max(0.0f, padding.w);
    const float padSumX = padL + padR;
    const float padSumY = padT + padB;
    if (padSumX <= 0.0f || padSumY <= 0.0f) {
        // No corner area at all — a single stretched quad.
        drawRect(clipped, textureHandle, uvRegion);
        return;
    }
    const float s  = std::min(1.0f, std::min(bW / padSumX, bH / padSumY));
    const float pl = padL * s, pr = padR * s, pt = padT * s, pb = padB * s;

    const float texW = static_cast<float>(ref->width);
    const float texH = static_cast<float>(ref->height);

    const float uSpan = std::max(0.0f, uvRegion.maxX - uvRegion.minX);
    const float vSpan = std::max(0.0f, uvRegion.maxY - uvRegion.minY);
    if (uSpan <= 0.0f || vSpan <= 0.0f) {
        drawRect(clipped, textureHandle, uvRegion);
        return;
    }

    // Texture-space paddings; clamped to the region span so the center UV
    // slice never inverts (degenerate tiny textures + huge pixel padding).
    float uPl = pl / texW, uPr = pr / texW;
    float uPt = pt / texH, uPb = pb / texH;
    if (uPl + uPr > uSpan) {
        const float su = uSpan / (uPl + uPr);
        uPl *= su;
        uPr *= su;
    }
    if (uPt + uPb > vSpan) {
        const float sv = vSpan / (uPt + uPb);
        uPt *= sv;
        uPb *= sv;
    }

    // 3x3 grid: 4 corners keep their size, edges stretch one axis, the
    // center stretches both. All 9 quads share (textureIdx, state) so they
    // form a single flush run.
    const float x[4] = {clipped.minX, clipped.minX + pl, clipped.maxX - pr, clipped.maxX};
    const float y[4] = {clipped.minY, clipped.minY + pt, clipped.maxY - pb, clipped.maxY};
    const float u[4] = {uvRegion.minX, uvRegion.minX + uPl,
                        uvRegion.maxX - uPr, uvRegion.maxX};
    const float v[4] = {uvRegion.minY, uvRegion.minY + uPt,
                        uvRegion.maxY - uPb, uvRegion.maxY};

    UiItem item;
    item.textureIdx = ref->textureIdx;
    item.state      = blendStateBits(_frame->currentBlend);
    const uint32_t abgr = 0xFFFFFFFFu;
    item.abgr[0] = abgr;
    item.abgr[1] = abgr;
    item.abgr[2] = abgr;
    item.abgr[3] = abgr;

    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            item.minX = x[i];
            item.minY = y[j];
            item.maxX = x[i + 1];
            item.maxY = y[j + 1];
            item.u0   = u[i];
            item.v0   = v[j];
            item.u1   = u[i + 1];
            item.v1   = v[j + 1];
            _frame->items.push_back(item);
        }
    }
}

void UIRenderBackend::flushColoredRects()
{
    if (_frame == nullptr) {
        return;
    }

    FrameState& frame = *_frame;
    if (frame.items.empty() || !_initialized || _gpu == nullptr || _adapter == nullptr
        || _width < 1 || _height < 1) {
        frame.items.clear();
        return;
    }

    const uint16_t whiteIdx  = _gpu->whiteTextureIdx();
    const float    fbW       = static_cast<float>(_width);
    const float    fbH       = static_cast<float>(_height);

    // Consecutive-runs only — never re-sort (z-order = array order).
    auto flushFlatRun = [&](size_t begin, size_t end) {
        const UiItem& first = frame.items[begin];
        const size_t  count = end - begin;

        frame.scratchVertices.clear();
        frame.scratchIndices.clear();
        frame.scratchVertices.reserve(count * 4u);
        frame.scratchIndices.reserve(count * 6u);

        for (size_t i = begin; i < end; ++i) {
            const UiItem& it = frame.items[i];
            const uint32_t base = static_cast<uint32_t>(frame.scratchVertices.size());
            const float    z    = 0.0f;

            frame.scratchVertices.push_back({toNdcX(it.minX, fbW), toNdcY(it.minY, fbH), z,
                                             it.abgr[0], it.u0, it.v0});
            frame.scratchVertices.push_back({toNdcX(it.maxX, fbW), toNdcY(it.minY, fbH), z,
                                             it.abgr[1], it.u1, it.v0});
            frame.scratchVertices.push_back({toNdcX(it.maxX, fbW), toNdcY(it.maxY, fbH), z,
                                             it.abgr[2], it.u1, it.v1});
            frame.scratchVertices.push_back({toNdcX(it.minX, fbW), toNdcY(it.maxY, fbH), z,
                                             it.abgr[3], it.u0, it.v1});

            frame.scratchIndices.push_back(base + 0);
            frame.scratchIndices.push_back(base + 1);
            frame.scratchIndices.push_back(base + 2);
            frame.scratchIndices.push_back(base + 0);
            frame.scratchIndices.push_back(base + 2);
            frame.scratchIndices.push_back(base + 3);
        }

        // P1: state is uniform within a run (grouping key), pass first.state.
        if (first.textureIdx == whiteIdx) {
            _gpu->submitColoredQuads(kViewId, *_adapter, first.state,
                                     frame.scratchVertices.data(),
                                     static_cast<uint32_t>(frame.scratchVertices.size()),
                                     sizeof(UiVertex), frame.scratchIndices.data(),
                                     static_cast<uint32_t>(frame.scratchIndices.size()));
        } else {
            _gpu->submitTexturedQuads(kViewId, *_adapter, first.state, first.textureIdx,
                                      frame.scratchVertices.data(),
                                      static_cast<uint32_t>(frame.scratchVertices.size()),
                                      sizeof(UiVertex), frame.scratchIndices.data(),
                                      static_cast<uint32_t>(frame.scratchIndices.size()));
        }
        ++_drawCalls;
    };

    size_t i = 0;
    const size_t n = frame.items.size();
    while (i < n) {
        const UiItem& it = frame.items[i];
        if (it.kind == UiItemKind::Sdf) {
            // P2: SDF items submit individually — the quad bounds already
            // include stroke/shadow expansion, all params ride via uniforms.
            // Fill is transparent (v_color0 = 0): the SDF path paints only
            // what its effect uniforms enable.
            frame.scratchVertices.clear();
            frame.scratchIndices.clear();
            frame.scratchVertices.reserve(4u);
            frame.scratchIndices.reserve(6u);
            const uint32_t base = static_cast<uint32_t>(frame.scratchVertices.size());
            const float    z    = 0.0f;
            const uint32_t transparent = 0u;

            frame.scratchVertices.push_back({toNdcX(it.minX, fbW), toNdcY(it.minY, fbH), z,
                                             transparent, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(it.maxX, fbW), toNdcY(it.minY, fbH), z,
                                             transparent, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(it.maxX, fbW), toNdcY(it.maxY, fbH), z,
                                             transparent, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(it.minX, fbW), toNdcY(it.maxY, fbH), z,
                                             transparent, 0.0f, 0.0f});
            frame.scratchIndices.push_back(base + 0);
            frame.scratchIndices.push_back(base + 1);
            frame.scratchIndices.push_back(base + 2);
            frame.scratchIndices.push_back(base + 0);
            frame.scratchIndices.push_back(base + 2);
            frame.scratchIndices.push_back(base + 3);

            _gpu->submitSdfQuads(kViewId, *_adapter, it.state,
                                 frame.scratchVertices.data(),
                                 static_cast<uint32_t>(frame.scratchVertices.size()),
                                 sizeof(UiVertex), frame.scratchIndices.data(),
                                 static_cast<uint32_t>(frame.scratchIndices.size()),
                                 it.sdf);
            ++_drawCalls;
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < n && frame.items[end].kind == UiItemKind::Flat
               && frame.items[end].textureIdx == it.textureIdx
               && frame.items[end].state == it.state) {
            ++end;
        }
        flushFlatRun(i, end);
        i = end;
    }

    frame.items.clear();
}

void UIRenderBackend::flushPendingText()
{
    // P0: text glyphs are UiItems in the unified batch; flushColoredRects
    // (called from endFrame / flushBatches) submits them. Kept as a
    // declared no-op so endFrame's call sequence stays unchanged.
}

void UIRenderBackend::drawTexturedQuad(const ayt::math::FRectangle& bounds, uint16_t textureIdx,
                                       const ayt::math::FVector4& tint)
{
    if (!_initialized || _gpu == nullptr || _adapter == nullptr || _width < 1 || _height < 1) {
        return;
    }

    const uint32_t abgr = toAbgr(tint);
    const UiVertex vertices[4] = {
        {toNdcX(bounds.minX, static_cast<float>(_width)),
         toNdcY(bounds.minY, static_cast<float>(_height)), 0.0f, abgr, 0.0f, 0.0f},
        {toNdcX(bounds.maxX, static_cast<float>(_width)),
         toNdcY(bounds.minY, static_cast<float>(_height)), 0.0f, abgr, 1.0f, 0.0f},
        {toNdcX(bounds.maxX, static_cast<float>(_width)),
         toNdcY(bounds.maxY, static_cast<float>(_height)), 0.0f, abgr, 1.0f, 1.0f},
        {toNdcX(bounds.minX, static_cast<float>(_width)),
         toNdcY(bounds.maxY, static_cast<float>(_height)), 0.0f, abgr, 0.0f, 1.0f},
    };
    const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};

    _gpu->submitTexturedQuads(kViewId, *_adapter, blendStateBits(ayt::ui::BlendMode::Normal),
                              textureIdx, vertices, 4, sizeof(UiVertex), indices, 6);
}

ayt::ui::IRenderBackend::TextMetrics UIRenderBackend::measureText(const std::wstring& text,
                                                                  int fontSize,
                                                                  float maxWidth) const
{
    TextMetrics out{0.0f, 0.0f, 0.0f, 0.0f};
    if (!_initialized || _fontAtlas == nullptr || fontSize < 8) {
        return out;
    }

    auto* atlas = const_cast<detail::BgfxFontAtlas*>(_fontAtlas.get());
    ayt::font::IFont* font = atlas->acquireFont(fontSize);
    if (font == nullptr) {
        return out;
    }

    const ayt::font::FontMetrics& fm = font->getMetrics();
    out.ascent  = fm.ascent;
    out.descent = fm.descent > 0.0f ? fm.descent : -fm.descent;
    out.height  = fm.lineHeight > 0.0f ? fm.lineHeight : static_cast<float>(fontSize);

    if (text.empty()) {
        return out;
    }

    const std::vector<ayt::font::ShapedGlyph> shaped = atlas->shapeText(font, text);
    if (!shaped.empty()) {
        out.width = atlas->measureShapedWidth(shaped);
    } else {
        out.width = font->measureText(text.c_str(), static_cast<int>(text.size()));
    }

    // maxWidth reserved for future wrap metrics; callers still get full span.
    AYUNREFERENCED_PARAM(maxWidth);
    return out;
}

ayt::font::FontMetrics UIRenderBackend::getFontMetrics(ayt::font::FontHandle font) const
{
    if (!_initialized || _fontAtlas == nullptr) {
        return ayt::font::FontMetrics{0, 0, 0, 0, 0, 0};
    }
    ayt::font::IFont* face = _fontAtlas->fontForHandle(font);
    if (face == nullptr) {
        return ayt::font::FontMetrics{0, 0, 0, 0, 0, 0};
    }
    return face->getMetrics();
}

ayt::font::FontHandle UIRenderBackend::getFontHandle(const wchar_t* /*familyName*/, int baseSize)
{
    if (!_initialized || _fontAtlas == nullptr) {
        return ayt::font::FontHandle{-1};
    }
    // Size-keyed UI fonts; family name is ignored until multi-face config lands.
    if (_fontAtlas->acquireFont(baseSize) == nullptr) {
        return ayt::font::FontHandle{-1};
    }
    return _fontAtlas->handleForSize(baseSize);
}

void UIRenderBackend::drawText(const ayt::math::FRectangle& bounds, const std::wstring& text,
                               int fontSize, const ayt::math::FVector4& color)
{
    if (!_initialized || text.empty() || _width < 1 || _height < 1 || _gpu == nullptr
        || _adapter == nullptr || _fontAtlas == nullptr) {
        return;
    }

    ayt::math::FRectangle clippedBounds = bounds;
    if (!clipRect(clippedBounds)) {
        return;
    }

    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    FrameState& frame = *_frame;

    ayt::font::IFont* font = _fontAtlas->acquireFont(fontSize);
    if (font == nullptr) {
        drawRect(bounds, color);
        return;
    }

    std::vector<ayt::font::ShapedGlyph> shaped = _fontAtlas->shapeText(font, text);
    const bool useShaped = !shaped.empty();
    if (useShaped) {
        _fontAtlas->prepareShapedGlyphs(font, fontSize, shaped);
    } else {
        _fontAtlas->prepareGlyphs(font, fontSize, text);
    }

    frame.textSyncFont = font;
    syncTextAtlasIfNeeded();

    const uint16_t atlasIdx = _fontAtlas->atlasTextureIdx();

    const ayt::font::FontMetrics& metrics = font->getMetrics();
    const float boundsH = bounds.maxY - bounds.minY;
    float cursorX       = bounds.minX;
    const float baselineY = bounds.minY + (boundsH - metrics.lineHeight) * 0.5f + metrics.ascent;

    const uint32_t abgr = toAbgr(color);
    const float    atlasW = static_cast<float>(detail::BgfxFontAtlas::kAtlasWidth);
    const float    atlasH = static_cast<float>(detail::BgfxFontAtlas::kAtlasHeight);

    auto emitGlyph = [&](ayt::font::GlyphInfo* glyph, float penX, float penY, float xOff,
                         float yOff, float xAdv) {
        if (glyph == nullptr) {
            cursorX = penX + xAdv;
            return;
        }

        const int glyphW = glyph->metrics.width;
        const int glyphH = glyph->metrics.height;
        if (glyphW <= 0 || glyphH <= 0) {
            cursorX = penX + xAdv;
            return;
        }

        const float x0 = penX + xOff + static_cast<float>(glyph->metrics.bearingX);
        // HB y_offset is up-positive; screen Y grows downward.
        const float y0 = penY - yOff - static_cast<float>(glyph->metrics.bearingY);
        const float x1 = x0 + static_cast<float>(glyphW);
        const float y1 = y0 + static_cast<float>(glyphH);

        ayt::math::FRectangle glyphBounds(x0, y0, x1, y1);
        if (!clipRect(glyphBounds)) {
            cursorX = penX + xAdv;
            return;
        }

        const float u0 = glyph->atlasPosX / atlasW;
        const float v0 = glyph->atlasPosY / atlasH;
        const float u1 = (glyph->atlasPosX + glyph->atlasWidth) / atlasW;
        const float v1 = (glyph->atlasPosY + glyph->atlasHeight) / atlasH;

        // Remap UVs when the quad is partially clipped so boundary glyphs
        // stay visible (cropped), instead of the old "drop if not fully
        // inside" path that made rows pop in/out at ScrollView edges.
        float tu0 = u0, tv0 = v0, tu1 = u1, tv1 = v1;
        const float gw = x1 - x0;
        const float gh = y1 - y0;
        if (gw > 1e-5f && gh > 1e-5f) {
            tu0 = u0 + (u1 - u0) * ((glyphBounds.minX - x0) / gw);
            tv0 = v0 + (v1 - v0) * ((glyphBounds.minY - y0) / gh);
            tu1 = u0 + (u1 - u0) * ((glyphBounds.maxX - x0) / gw);
            tv1 = v0 + (v1 - v0) * ((glyphBounds.maxY - y0) / gh);
        }

        // P0 unified batch: one UiItem per glyph quad; all glyphs of a
        // drawText call share (atlas, state) so they form one flush run.
        UiItem item;
        item.textureIdx = atlasIdx;
        item.state = blendStateBits(_frame->currentBlend);  // P1
        item.minX = glyphBounds.minX;
        item.minY = glyphBounds.minY;
        item.maxX = glyphBounds.maxX;
        item.maxY = glyphBounds.maxY;
        item.abgr[0] = abgr;
        item.abgr[1] = abgr;
        item.abgr[2] = abgr;
        item.abgr[3] = abgr;
        item.u0 = tu0;
        item.v0 = tv0;
        item.u1 = tu1;
        item.v1 = tv1;
        frame.items.push_back(item);
        cursorX = penX + xAdv;
    };

    if (useShaped) {
        for (const ayt::font::ShapedGlyph& sg : shaped) {
            ayt::font::GlyphInfo* glyph = font->getGlyphByIndex(sg.glyphIndex);
            const float xOff = static_cast<float>(sg.xOffset) / 64.0f;
            const float yOff = static_cast<float>(sg.yOffset) / 64.0f;
            const float xAdv = static_cast<float>(sg.xAdvance) / 64.0f;
            emitGlyph(glyph, cursorX, baselineY, xOff, yOff, xAdv);
        }
    } else {
        for (wchar_t ch : text) {
            ayt::font::GlyphInfo* glyph = font->getGlyph(static_cast<uint32_t>(ch));
            const float xAdv = (glyph != nullptr)
                                   ? static_cast<float>(glyph->metrics.advance)
                                   : metrics.lineHeight * 0.25f;
            emitGlyph(glyph, cursorX, baselineY, 0.0f, 0.0f, xAdv);
        }
    }
}

} // namespace ayt::render
