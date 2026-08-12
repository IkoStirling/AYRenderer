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
#include <cstring>
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
    // SDF only: shape center + half-extent (replaces the u_rect uniform so
    // same-param SDF items can share a submission). Flat/text vertices
    // leave these 0 — their shaders never read TexCoord1.
    float    shapeCx;
    float    shapeCy;
    float    shapeHalfW;
    float    shapeHalfH;
    // SDF only: clip rect as (center, half-extent), soft-clip seam AA.
    // The draw quad is already CPU-intersected with the clip; this lets
    // the SDF shader fade coverage across the clip edge instead of
    // hard-cutting stroke/shadow AA gradients (scroll edges). Flat/text
    // vertices leave these 0 — their shaders never read TexCoord2.
    float    clipCx;
    float    clipCy;
    float    clipHalfW;
    float    clipHalfH;
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
    // Shape silhouette (Sdf only): the *content* rect. Differs from the
    // draw quad (minX..maxY, expanded by shadow/outside-stroke/blur).
    // Baked into vertices as (center, half-extent) at flush so SDF items
    // with identical sdf params can share one submission (run merging).
    float      shapeMinX    = 0.0f;
    float      shapeMinY    = 0.0f;
    float      shapeMaxX    = 0.0f;
    float      shapeMaxY    = 0.0f;
    // SDF only: the clip rect at record time (activeClipBounds — full
    // screen when no clip is pushed). Soft-clip seam AA, see UiVertex.
    float      clipMinX     = 0.0f;
    float      clipMinY     = 0.0f;
    float      clipMaxX     = 0.0f;
    float      clipMaxY     = 0.0f;
};                                          //     (minX..maxY) already expanded

// SdfParams holds FVector4 members (16B-aligned f128) → trailing padding;
// memcmp would compare uninitialized tail bytes and never match. Compare
// the value fields only (the run-merge key for SDF batching).
bool sdfParamsEqual(const detail::UiGpuContext::SdfParams& a,
                    const detail::UiGpuContext::SdfParams& b)
{
    return a.radius == b.radius && a.strokeColor == b.strokeColor
        && a.strokeWidth == b.strokeWidth && a.strokeInset == b.strokeInset
        && a.shadowColor == b.shadowColor && a.shadowOffset == b.shadowOffset
        && a.shadowBlur == b.shadowBlur;
}

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
    // PR-anim: stacked global opacity (LIFO, mirrors clipStack). Every
    // color-emitting entry multiplies its alpha by the stack top; the
    // base frame is 1.0 so default rendering is byte-identical.
    std::vector<float> opacityStack = {1.0f};
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
    // PR-anim: opacity is a per-frame render-state like BlendMode —
    // Widget::render balances its push/pop every frame, but a buggy
    // tree must not leak a stale frame into the next frame.
    frame.opacityStack.assign(1, 1.0f);

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
    // PR-anim: flat fill fades with the tree.
    ayt::math::FVector4 fill = color;
    fill.w *= _frame->opacityStack.back();
    const uint32_t abgr = toAbgr(fill);
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

void UIRenderBackend::pushOpacity(float alpha)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }
    // Clamp to [0,1]: 1.0 is a no-op, out-of-range values are caller
    // bugs that would otherwise stack nonsense (alpha > 1 makes draws
    // opaque-er than authored, negative alpha breaks the multiply).
    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    // COMPOUND: the stack stores the CUMULATIVE product, not the raw
    // frame — parent push(0.5) then child push(0.5) yields stack top
    // 0.25 (tree opacity multiplies). pop() restores the parent frame.
    _frame->opacityStack.push_back(_frame->opacityStack.back() * a);
}

void UIRenderBackend::popOpacity()
{
    if (_frame == nullptr) {
        return;
    }
    // The stack base (1.0) is never popped — an unbalanced pop must not
    // empty the stack and corrupt the rest of the frame.
    if (_frame->opacityStack.size() > 1) {
        _frame->opacityStack.pop_back();
    }
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
    // PR-anim: gradient corners fade with the tree.
    ayt::math::FVector4 tl = topLeft, tr = topRight, bl = bottomLeft, br = bottomRight;
    const float op = _frame->opacityStack.back();
    tl.w *= op; tr.w *= op; bl.w *= op; br.w *= op;
    item.abgr[0] = toAbgr(tl);
    item.abgr[1] = toAbgr(tr);
    item.abgr[2] = toAbgr(br);
    item.abgr[3] = toAbgr(bl);
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

    // Shape stays the caller bounds. Only the *draw quad* is intersected
    // with the clip — reshaping sdf.rect to the clipped remnant invents
    // four new corners that "stick" to the scroll/clip edge.
    ayt::math::FRectangle drawQuad(bounds.minX - w, bounds.minY - w,
                                   bounds.maxX + w, bounds.maxY + w);
    if (!clipRect(drawQuad)) {
        return;
    }

    const float maxRadius = std::min(width, height) * 0.5f;
    const float r         = std::max(0.0f, std::min(cornerRadius, maxRadius));

    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = drawQuad.minX;
    item.minY  = drawQuad.minY;
    item.maxX  = drawQuad.maxX;
    item.maxY  = drawQuad.maxY;
    item.shapeMinX = bounds.minX;
    item.shapeMinY = bounds.minY;
    item.shapeMaxX = bounds.maxX;
    item.shapeMaxY = bounds.maxY;
    item.sdf.radius      = ayt::math::FVector4(r, r, r, r);
    // PR-anim: stroke fades with the tree (shader treats a=0 as no stroke).
    ayt::math::FVector4 stroke = color;
    stroke.w *= _frame->opacityStack.back();
    item.sdf.strokeColor = stroke;
    item.sdf.strokeWidth = w;
    item.sdf.strokeInset = 0.0f;
    // Soft-clip: the clip rect at record time, for seam AA in the shader.
    const ayt::math::FRectangle clip = activeClipBounds();
    item.clipMinX = clip.minX;
    item.clipMinY = clip.minY;
    item.clipMaxX = clip.maxX;
    item.clipMaxY = clip.maxY;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawRoundedRect(const ayt::math::FRectangle& bounds,
                                      const ayt::math::FVector4& color,
                                      float cornerRadius)
{
    // SDF filled rounded rect — pairs with drawBorderRect so combo cards
    // (fill + stroke) share the same silhouette. Flat drawRect is axis-
    // aligned and pokes square corners through a rounded stroke.
    if (color.w <= 0.0f) {
        return;
    }
    if (cornerRadius <= 0.0f) {
        drawRect(bounds, color);
        return;
    }

    const float width  = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    constexpr float kAa = 1.0f;
    ayt::math::FRectangle drawQuad(bounds.minX - kAa, bounds.minY - kAa,
                                   bounds.maxX + kAa, bounds.maxY + kAa);
    if (!clipRect(drawQuad)) {
        return;
    }

    const float maxRadius = std::min(width, height) * 0.5f;
    const float r         = std::max(0.0f, std::min(cornerRadius, maxRadius));

    // PR-anim: SDF fill fades with the tree (per-vertex color rides alpha).
    ayt::math::FVector4 fillColor = color;
    fillColor.w *= _frame->opacityStack.back();
    const uint32_t fill = toAbgr(fillColor);
    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = drawQuad.minX;
    item.minY  = drawQuad.minY;
    item.maxX  = drawQuad.maxX;
    item.maxY  = drawQuad.maxY;
    item.abgr[0] = fill;
    item.abgr[1] = fill;
    item.abgr[2] = fill;
    item.abgr[3] = fill;
    // Original silhouette — do not reshape to the clipped remnant.
    item.shapeMinX = bounds.minX;
    item.shapeMinY = bounds.minY;
    item.shapeMaxX = bounds.maxX;
    item.shapeMaxY = bounds.maxY;
    item.sdf.radius = ayt::math::FVector4(r, r, r, r);
    const ayt::math::FRectangle clip = activeClipBounds();
    item.clipMinX = clip.minX;
    item.clipMinY = clip.minY;
    item.clipMaxX = clip.maxX;
    item.clipMaxY = clip.maxY;
    _frame->items.push_back(item);
}

void UIRenderBackend::drawRectShadow(const ayt::math::FRectangle& bounds,
                                     const ayt::ui::IRenderBackend::ShadowStyle& shadow)
{
    // P2: single SDF item; blur softens coverage over `blur` pixels in the
    // shader (d/blur), not by expanding the shape rect. The draw quad grows
    // by |offset| + blur so the falloff is not clipped. The shape fields
    // stay the *content* bounds — using the expanded outer made solid
    // black blobs.
    if (shadow.color.w <= 0.0f) {
        return;
    }

    const float width  = bounds.maxX - bounds.minX;
    const float height = bounds.maxY - bounds.minY;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }

    const float blur = std::max(0.0f, shadow.blurRadius);
    const float extX = std::fabs(shadow.offset.x) + blur;
    const float extY = std::fabs(shadow.offset.y) + blur;

    ayt::math::FRectangle drawQuad(bounds.minX - extX, bounds.minY - extY,
                                   bounds.maxX + extX, bounds.maxY + extY);
    if (!clipRect(drawQuad)) {
        return;
    }

    const float maxRadius = std::min(width, height) * 0.5f;
    const float r         = std::max(0.0f, std::min(shadow.cornerRadius, maxRadius));

    UiItem item;
    item.kind  = UiItemKind::Sdf;
    item.state = blendStateBits(_frame->currentBlend);
    item.minX  = drawQuad.minX;
    item.minY  = drawQuad.minY;
    item.maxX  = drawQuad.maxX;
    item.maxY  = drawQuad.maxY;
    item.shapeMinX = bounds.minX;
    item.shapeMinY = bounds.minY;
    item.shapeMaxX = bounds.maxX;
    item.shapeMaxY = bounds.maxY;
    item.sdf.radius       = ayt::math::FVector4(r, r, r, r);
    // PR-anim: shadow fades with the tree (a=0 = no shadow in shader).
    ayt::math::FVector4 shadowColor = shadow.color;
    shadowColor.w *= _frame->opacityStack.back();
    item.sdf.shadowColor  = shadowColor;
    item.sdf.shadowOffset = shadow.offset;
    item.sdf.shadowBlur   = blur;
    const ayt::math::FRectangle clip = activeClipBounds();
    item.clipMinX = clip.minX;
    item.clipMinY = clip.minY;
    item.clipMaxX = clip.maxX;
    item.clipMaxY = clip.maxY;
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

    // Flat item on the UI texture with an opaque white tint — the shader
    // multiplies v_color0 by the texture sample, so alpha rides through.
    emitClippedTexturedQuad(bounds, ref->textureIdx, uv,
                            ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
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

    // Interface contract: alpha multiplies the texture's alpha channel —
    // carried by the tint (shader does v_color0 * sample).
    emitClippedTexturedQuad(bounds, ref->textureIdx,
                            ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                            ayt::math::FVector4(
                                1.0f, 1.0f, 1.0f, std::max(0.0f, std::min(1.0f, alpha))));
}

void UIRenderBackend::emitClippedTexturedQuad(const ayt::math::FRectangle& bounds,
                                              uint16_t textureIdx,
                                              const ayt::math::FRectangle& uv,
                                              const ayt::math::FVector4& tint)
{
    if (_frame == nullptr) {
        _frame = std::make_unique<FrameState>();
    }

    ayt::math::FRectangle clipped = bounds;
    if (!clipRect(clipped)) {
        return;
    }

    // UV remap: the quad is CPU-clipped to the clip rect, so the sampled
    // region must shrink by the same fraction — otherwise the texture
    // smears/stretches across the seam (whole-image UVs on a clipped quad
    // squeeze the edge texels). Correct for LINEAR sampling (UI textures);
    // glyph UVs never route here (they remap in emitGlyph already).
    float u0 = uv.minX, v0 = uv.minY, u1 = uv.maxX, v1 = uv.maxY;
    const float bw = bounds.maxX - bounds.minX;
    const float bh = bounds.maxY - bounds.minY;
    if (bw > 1e-5f && bh > 1e-5f) {
        const float lf = (clipped.minX - bounds.minX) / bw;
        const float rf = (clipped.maxX - bounds.minX) / bw;
        const float tf = (clipped.minY - bounds.minY) / bh;
        const float bf = (clipped.maxY - bounds.minY) / bh;
        u0 = uv.minX + (uv.maxX - uv.minX) * lf;
        u1 = uv.minX + (uv.maxX - uv.minX) * rf;
        v0 = uv.minY + (uv.maxY - uv.minY) * tf;
        v1 = uv.minY + (uv.maxY - uv.minY) * bf;
    }

    // PR-anim: alpha rides the tint (shader does v_color0 * sample);
    // opacity==1 keeps 0xFFFFFFFF.
    const uint32_t abgr = toAbgr(ayt::math::FVector4(
        tint.x, tint.y, tint.z, tint.w * _frame->opacityStack.back()));
    UiItem item;
    item.textureIdx = textureIdx;
    item.state      = blendStateBits(_frame->currentBlend);
    item.minX       = clipped.minX;
    item.minY       = clipped.minY;
    item.maxX       = clipped.maxX;
    item.maxY       = clipped.maxY;
    item.abgr[0] = abgr;
    item.abgr[1] = abgr;
    item.abgr[2] = abgr;
    item.abgr[3] = abgr;
    item.u0 = u0;
    item.v0 = v0;
    item.u1 = u1;
    item.v1 = v1;
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

    // 9-slice layout is computed from the *unclipped* bounds — a clip
    // should crop the display, not re-layout the slices (the old code
    // intersected first, which also shrunk the corner-fit scale `s`).
    // Each slice is clipped + UV-remapped by emitClippedTexturedQuad.
    const float bW = bounds.maxX - bounds.minX;
    const float bH = bounds.maxY - bounds.minY;
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
        emitClippedTexturedQuad(bounds, ref->textureIdx, uvRegion,
                                ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
        return;
    }
    const float s  = std::min(1.0f, std::min(bW / padSumX, bH / padSumY));
    const float pl = padL * s, pr = padR * s, pt = padT * s, pb = padB * s;

    const float texW = static_cast<float>(ref->width);
    const float texH = static_cast<float>(ref->height);

    const float uSpan = std::max(0.0f, uvRegion.maxX - uvRegion.minX);
    const float vSpan = std::max(0.0f, uvRegion.maxY - uvRegion.minY);
    if (uSpan <= 0.0f || vSpan <= 0.0f) {
        emitClippedTexturedQuad(bounds, ref->textureIdx, uvRegion,
                                ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
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
    // form a single flush run; each is individually clipped + UV-remapped.
    const float x[4] = {bounds.minX, bounds.minX + pl, bounds.maxX - pr, bounds.maxX};
    const float y[4] = {bounds.minY, bounds.minY + pt, bounds.maxY - pb, bounds.maxY};
    const float u[4] = {uvRegion.minX, uvRegion.minX + uPl,
                        uvRegion.maxX - uPr, uvRegion.maxX};
    const float v[4] = {uvRegion.minY, uvRegion.minY + uPt,
                        uvRegion.maxY - uPb, uvRegion.maxY};

    const ayt::math::FVector4 white(1.0f, 1.0f, 1.0f, 1.0f);
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            emitClippedTexturedQuad(ayt::math::FRectangle(x[i], y[j], x[i + 1], y[j + 1]),
                                    ref->textureIdx,
                                    ayt::math::FRectangle(u[i], v[j], u[i + 1], v[j + 1]),
                                    white);
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
                                             it.abgr[0], it.u0, it.v0,
                                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(it.maxX, fbW), toNdcY(it.minY, fbH), z,
                                             it.abgr[1], it.u1, it.v0,
                                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(it.maxX, fbW), toNdcY(it.maxY, fbH), z,
                                             it.abgr[2], it.u1, it.v1,
                                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
            frame.scratchVertices.push_back({toNdcX(it.minX, fbW), toNdcY(it.maxY, fbH), z,
                                             it.abgr[3], it.u0, it.v1,
                                             0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});

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
            // Batch knife: consecutive SDF items with IDENTICAL params
            // (shape rect rides per-vertex now, so position/color vary
            // freely within a run) and same blend state merge into one
            // submission. Per-item uniforms would force one call per
            // quad; same-skin buttons/panels collapse to a single call.
            // Never re-sort (z-order = array order).
            size_t end = i + 1;
            while (end < n && frame.items[end].kind == UiItemKind::Sdf
                   && frame.items[end].state == it.state
                   && sdfParamsEqual(frame.items[end].sdf, it.sdf)) {
                ++end;
            }

            frame.scratchVertices.clear();
            frame.scratchIndices.clear();
            frame.scratchVertices.reserve((end - i) * 4u);
            frame.scratchIndices.reserve((end - i) * 6u);
            const float z = 0.0f;

            for (size_t k = i; k < end; ++k) {
                const UiItem& s = frame.items[k];
                const uint32_t base = static_cast<uint32_t>(frame.scratchVertices.size());
                // Fill from abgr[0] (drawRoundedRect); borders leave it 0.
                const uint32_t fill = s.abgr[0];
                const float cx = (s.shapeMinX + s.shapeMaxX) * 0.5f;
                const float cy = (s.shapeMinY + s.shapeMaxY) * 0.5f;
                const float hw = (s.shapeMaxX - s.shapeMinX) * 0.5f;
                const float hh = (s.shapeMaxY - s.shapeMinY) * 0.5f;
                // Soft-clip rect as (center, half-extent); all-zero
                // (chw == 0) = no clip, shader skips the seam fade.
                float ccx = 0.0f, ccy = 0.0f, chw = 0.0f, chh = 0.0f;
                if (s.clipMaxX > s.clipMinX && s.clipMaxY > s.clipMinY) {
                    ccx = (s.clipMinX + s.clipMaxX) * 0.5f;
                    ccy = (s.clipMinY + s.clipMaxY) * 0.5f;
                    chw = (s.clipMaxX - s.clipMinX) * 0.5f;
                    chh = (s.clipMaxY - s.clipMinY) * 0.5f;
                }
                // a_texcoord0 / v_pos MUST be draw-quad pixel corners
                // (minX..maxY), not shapeMin/Max. Shape is often inset
                // (outside stroke / shadow blur expand the draw quad);
                // packing shape corners warps SDF space so stroke rings
                // collapse to corner arcs and soft shadows become solid
                // blocks. Shape silhouette stays in TexCoord1 (cx,cy,hw,hh).
                frame.scratchVertices.push_back({toNdcX(s.minX, fbW), toNdcY(s.minY, fbH), z,
                                                 fill, s.minX, s.minY, cx, cy, hw, hh,
                                                 ccx, ccy, chw, chh});
                frame.scratchVertices.push_back({toNdcX(s.maxX, fbW), toNdcY(s.minY, fbH), z,
                                                 fill, s.maxX, s.minY, cx, cy, hw, hh,
                                                 ccx, ccy, chw, chh});
                frame.scratchVertices.push_back({toNdcX(s.maxX, fbW), toNdcY(s.maxY, fbH), z,
                                                 fill, s.maxX, s.maxY, cx, cy, hw, hh,
                                                 ccx, ccy, chw, chh});
                frame.scratchVertices.push_back({toNdcX(s.minX, fbW), toNdcY(s.maxY, fbH), z,
                                                 fill, s.minX, s.maxY, cx, cy, hw, hh,
                                                 ccx, ccy, chw, chh});
                frame.scratchIndices.push_back(base + 0);
                frame.scratchIndices.push_back(base + 1);
                frame.scratchIndices.push_back(base + 2);
                frame.scratchIndices.push_back(base + 0);
                frame.scratchIndices.push_back(base + 2);
                frame.scratchIndices.push_back(base + 3);
            }

            _gpu->submitSdfQuads(kViewId, *_adapter, it.state,
                                 frame.scratchVertices.data(),
                                 static_cast<uint32_t>(frame.scratchVertices.size()),
                                 sizeof(UiVertex), frame.scratchIndices.data(),
                                 static_cast<uint32_t>(frame.scratchIndices.size()),
                                 it.sdf);
            ++_drawCalls;
            i = end;
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
         toNdcY(bounds.minY, static_cast<float>(_height)), 0.0f, abgr, 0.0f, 0.0f,
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {toNdcX(bounds.maxX, static_cast<float>(_width)),
         toNdcY(bounds.minY, static_cast<float>(_height)), 0.0f, abgr, 1.0f, 0.0f,
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {toNdcX(bounds.maxX, static_cast<float>(_width)),
         toNdcY(bounds.maxY, static_cast<float>(_height)), 0.0f, abgr, 1.0f, 1.0f,
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {toNdcX(bounds.minX, static_cast<float>(_width)),
         toNdcY(bounds.maxY, static_cast<float>(_height)), 0.0f, abgr, 0.0f, 1.0f,
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
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

    // Glyph advances (px). Shaped path = HB 26.6 fixed-point; fallback =
    // per-codepoint advance (same default advance the draw path uses).
    const std::vector<ayt::font::ShapedGlyph> shaped = atlas->shapeText(font, text);
    const size_t n = shaped.empty() ? text.size() : shaped.size();
    std::vector<float> advances;
    std::vector<bool>  isSpace;
    advances.reserve(n);
    isSpace.reserve(n);
    if (!shaped.empty()) {
        for (const ayt::font::ShapedGlyph& sg : shaped) {
            advances.push_back(static_cast<float>(sg.xAdvance) / 64.0f);
            const size_t ci = std::min<size_t>(sg.charIndex, text.size() - 1);
            isSpace.push_back(text[ci] == L' ' || text[ci] == L'\t');
        }
    } else {
        for (wchar_t ch : text) {
            ayt::font::GlyphInfo* glyph = font->getGlyph(static_cast<uint32_t>(ch));
            advances.push_back((glyph != nullptr) ? static_cast<float>(glyph->metrics.advance)
                                                  : out.height * 0.25f);
            isSpace.push_back(ch == L' ' || ch == L'\t');
        }
    }

    if (maxWidth <= 0.0f) {
        // No wrap constraint: single line, full span.
        for (float a : advances) {
            out.width += a;
        }
        return out;
    }

    // Greedy word wrap (maxWidth px): break at the last space that fits;
    // a word wider than maxWidth hard-breaks per glyph (CJK has no
    // spaces, so it degrades to per-glyph breaking naturally).
    const float lh = out.height > 0.0f ? out.height : static_cast<float>(fontSize);
    std::vector<float> prefix(n + 1, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + advances[i];
    }

    int    lines     = 1;
    float  lineW     = 0.0f;
    float  maxLineW  = 0.0f;
    size_t lineStart = 0;
    size_t lastBreak = n;  // index of the last space that fit in the line; n = none
    for (size_t i = 0; i < n; ++i) {
        const float w = advances[i];
        if (lineW + w <= maxWidth || lineW <= 0.0f) {
            lineW += w;
            if (isSpace[i]) {
                lastBreak = i;
            }
        } else {
            // Line full. Break after the last fitting space when one is in
            // this line; the space itself stays on the old line, so the new
            // line's width = glyphs (lastBreak+1 .. i]. Else hard-break.
            if (lastBreak != n && lastBreak >= lineStart) {
                lineW = prefix[i + 1] - prefix[lastBreak + 1];
                lineStart = lastBreak + 1;
                lastBreak = n;
            } else {
                lineW     = w;
                lineStart = i;
            }
            if (isSpace[i]) {
                lastBreak = i;
            }
            ++lines;
        }
        maxLineW = std::max(maxLineW, lineW);
    }

    out.width  = maxLineW;
    out.height = static_cast<float>(lines) * lh;
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
    // Style route: default style keeps the legacy single-line behavior
    // (Align::Left + VAlign::Middle == the old centered baseline).
    ayt::ui::IRenderBackend::TextStyle style;
    style.color = color;
    drawText(bounds, text, fontSize, style);
}

void UIRenderBackend::drawText(const ayt::math::FRectangle& bounds, const std::wstring& text,
                               int fontSize, const ayt::ui::IRenderBackend::TextStyle& style)
{
    if (!_initialized || text.empty() || _width < 1 || _height < 1 || _gpu == nullptr
        || _adapter == nullptr || _fontAtlas == nullptr) {
        return;
    }

    // outline / shadow / letterSpacing / lineSpacing: documented as
    // not-yet-implemented (the interface default previously dropped them
    // too). This implementation honors style.color + style.align +
    // style.valign. Single-line only — callers wrap via measureText and
    // draw per line.

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
        drawRect(bounds, style.color);
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
    const float boundsW = bounds.maxX - bounds.minX;
    const float boundsH = bounds.maxY - bounds.minY;

    // Full-line width first so horizontal alignment can position the
    // whole line (same advance math as measureText).
    float lineWidth = 0.0f;
    if (useShaped) {
        for (const ayt::font::ShapedGlyph& sg : shaped) {
            lineWidth += static_cast<float>(sg.xAdvance) / 64.0f;
        }
    } else {
        for (wchar_t ch : text) {
            ayt::font::GlyphInfo* glyph = font->getGlyph(static_cast<uint32_t>(ch));
            lineWidth += (glyph != nullptr) ? static_cast<float>(glyph->metrics.advance)
                                            : metrics.lineHeight * 0.25f;
        }
    }

    float cursorX = uiTextAlignX(style.align, bounds.minX, boundsW, lineWidth);
    const float baselineY =
        uiTextBaselineY(style.valign, bounds.minY, boundsH, metrics.lineHeight, metrics.ascent);

    // PR-anim: glyph color fades with the tree.
    ayt::math::FVector4 glyphColor = style.color;
    glyphColor.w *= _frame->opacityStack.back();
    const uint32_t abgr = toAbgr(glyphColor);
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
