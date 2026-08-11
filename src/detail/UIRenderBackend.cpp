#include "AYUIRenderBackend.h"

#include "AYRenderer.h"
#include "detail/BgfxFontAtlas.h"
#include "detail/BGFXAdapter.h"
#include "detail/UiGpuContext.h"
#include "AYShaderResourcePool.h"

#include <AYCoreUtility.h>

#include <algorithm>
#include <cstdio>
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

struct ColoredRect {
    ayt::math::FRectangle bounds;
    ayt::math::FVector4   color;
};

struct PendingTextBatch {
    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
};

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

} // namespace

struct UIRenderBackend::FrameState {
    std::vector<ColoredRect>          pendingRects;
    std::vector<UiVertex>             scratchVertices;
    std::vector<uint32_t>             scratchIndices;
    std::unique_ptr<PendingTextBatch> textBatch;
    ayt::font::IFont*                 textSyncFont = nullptr;
    std::vector<ayt::math::FRectangle> clipStack;
};

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
        _frame->pendingRects.clear();
        _frame->scratchVertices.clear();
        _frame->scratchIndices.clear();
        if (_frame->textBatch != nullptr) {
            _frame->textBatch->vertices.clear();
            _frame->textBatch->indices.clear();
        }
        _frame->textSyncFont = nullptr;
        _frame->textBatch.reset();
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
        _frame->pendingRects.clear();
        _frame->scratchVertices.clear();
        _frame->scratchIndices.clear();
        if (_frame->textBatch != nullptr) {
            _frame->textBatch->vertices.clear();
            _frame->textBatch->indices.clear();
        }
        _frame->textSyncFont = nullptr;
        _frame->textBatch.reset();
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
    frame.pendingRects.clear();
    frame.clipStack.clear();
    frame.textSyncFont = nullptr;

    if (frame.scratchVertices.capacity() < 1024u) {
        frame.scratchVertices.reserve(1024u);
        frame.scratchIndices.reserve(1536u);
    }

    if (frame.textBatch == nullptr) {
        frame.textBatch = std::make_unique<PendingTextBatch>();
    }
    frame.textBatch->vertices.clear();
    frame.textBatch->indices.clear();
    if (frame.textBatch->vertices.capacity() < 512u) {
        frame.textBatch->vertices.reserve(512u);
        frame.textBatch->indices.reserve(768u);
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
    flushColoredRects();
    flushPendingText();

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
    flushColoredRects();
    flushPendingText();
    _frame->clipStack.pop_back();
}

void UIRenderBackend::endFrame()
{
    flushColoredRects();
    flushPendingText();
}

void UIRenderBackend::flushBatches()
{
    flushPendingText();
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

    // Cut the text batch before any new rect so overlays drawn after text
    // keep correct z-order (rect submit follows flushed text).
    if (_frame->textBatch != nullptr && !_frame->textBatch->vertices.empty()) {
        flushPendingText();
    }

    _frame->pendingRects.push_back({clipped, color});
}

void UIRenderBackend::drawRect(const ayt::math::FRectangle& bounds, void* textureHandle,
                               const ayt::math::FRectangle& uv)
{
    AYUNREFERENCED_PARAM(uv);
    AYUNREFERENCED_PARAM(textureHandle);
    drawRect(bounds, ayt::math::FVector4(0.25f, 0.25f, 0.28f, 1.0f));
}

void UIRenderBackend::drawWithAlpha(const ayt::math::FRectangle& bounds, void* textureHandle,
                                    float alpha)
{
    drawRect(bounds, textureHandle, ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    AYUNREFERENCED_PARAM(alpha);
}

void UIRenderBackend::flushColoredRects()
{
    if (_frame == nullptr) {
        return;
    }

    FrameState& frame = *_frame;
    if (frame.pendingRects.empty() || !_initialized || _gpu == nullptr || _adapter == nullptr
        || _width < 1 || _height < 1) {
        frame.pendingRects.clear();
        return;
    }

    frame.scratchVertices.clear();
    frame.scratchIndices.clear();
    frame.scratchVertices.reserve(frame.pendingRects.size() * 4u);
    frame.scratchIndices.reserve(frame.pendingRects.size() * 6u);

    for (const ColoredRect& rect : frame.pendingRects) {
        const uint32_t base = static_cast<uint32_t>(frame.scratchVertices.size());
        const uint32_t abgr = toAbgr(rect.color);
        const float    z    = 0.0f;

        frame.scratchVertices.push_back({toNdcX(rect.bounds.minX, static_cast<float>(_width)),
                                         toNdcY(rect.bounds.minY, static_cast<float>(_height)), z,
                                         abgr, 0.0f, 0.0f});
        frame.scratchVertices.push_back({toNdcX(rect.bounds.maxX, static_cast<float>(_width)),
                                         toNdcY(rect.bounds.minY, static_cast<float>(_height)), z,
                                         abgr, 1.0f, 0.0f});
        frame.scratchVertices.push_back({toNdcX(rect.bounds.maxX, static_cast<float>(_width)),
                                         toNdcY(rect.bounds.maxY, static_cast<float>(_height)), z,
                                         abgr, 1.0f, 1.0f});
        frame.scratchVertices.push_back({toNdcX(rect.bounds.minX, static_cast<float>(_width)),
                                         toNdcY(rect.bounds.maxY, static_cast<float>(_height)), z,
                                         abgr, 0.0f, 1.0f});

        frame.scratchIndices.push_back(base + 0);
        frame.scratchIndices.push_back(base + 1);
        frame.scratchIndices.push_back(base + 2);
        frame.scratchIndices.push_back(base + 0);
        frame.scratchIndices.push_back(base + 2);
        frame.scratchIndices.push_back(base + 3);
    }

    _gpu->submitColoredQuads(kViewId, *_adapter, frame.scratchVertices.data(),
                             static_cast<uint32_t>(frame.scratchVertices.size()), sizeof(UiVertex),
                             frame.scratchIndices.data(),
                             static_cast<uint32_t>(frame.scratchIndices.size()));

    ++_drawCalls;
    frame.pendingRects.clear();
}

void UIRenderBackend::flushPendingText()
{
    if (_frame == nullptr) {
        return;
    }

    FrameState& frame = *_frame;
    if (frame.textBatch == nullptr || frame.textBatch->vertices.empty() || !_initialized
        || _gpu == nullptr || _adapter == nullptr || _fontAtlas == nullptr || _width < 1
        || _height < 1) {
        if (frame.textBatch != nullptr) {
            frame.textBatch->vertices.clear();
            frame.textBatch->indices.clear();
        }
        return;
    }

    syncTextAtlasIfNeeded();

    const uint16_t atlasIdx = _fontAtlas->atlasTextureIdx();
    if (atlasIdx == detail::UiGpuContext::kInvalidIdx) {
        frame.textBatch->vertices.clear();
        frame.textBatch->indices.clear();
        return;
    }

    _gpu->submitTexturedQuads(kViewId, *_adapter, atlasIdx, frame.textBatch->vertices.data(),
                              static_cast<uint32_t>(frame.textBatch->vertices.size()),
                              sizeof(UiVertex), frame.textBatch->indices.data(),
                              static_cast<uint32_t>(frame.textBatch->indices.size()));
    ++_drawCalls;
    frame.textBatch->vertices.clear();
    frame.textBatch->indices.clear();
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

    _gpu->submitTexturedQuads(kViewId, *_adapter, textureIdx, vertices, 4, sizeof(UiVertex),
                              indices, 6);
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
    flushColoredRects();

    if (frame.textBatch == nullptr) {
        frame.textBatch = std::make_unique<PendingTextBatch>();
    }

    const ayt::font::FontMetrics& metrics = font->getMetrics();
    const float boundsH = bounds.maxY - bounds.minY;
    float cursorX       = bounds.minX;
    const float baselineY = bounds.minY + (boundsH - metrics.lineHeight) * 0.5f + metrics.ascent;

    const uint32_t abgr = toAbgr(color);
    const float    atlasW = static_cast<float>(detail::BgfxFontAtlas::kAtlasWidth);
    const float    atlasH = static_cast<float>(detail::BgfxFontAtlas::kAtlasHeight);
    const float    fbW    = static_cast<float>(_width);
    const float    fbH    = static_cast<float>(_height);

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

        const uint32_t base = static_cast<uint32_t>(frame.textBatch->vertices.size());
        const float    z    = 0.0f;
        const float qx0 = glyphBounds.minX;
        const float qy0 = glyphBounds.minY;
        const float qx1 = glyphBounds.maxX;
        const float qy1 = glyphBounds.maxY;
        frame.textBatch->vertices.push_back({toNdcX(qx0, fbW), toNdcY(qy0, fbH), z, abgr, tu0, tv0});
        frame.textBatch->vertices.push_back({toNdcX(qx1, fbW), toNdcY(qy0, fbH), z, abgr, tu1, tv0});
        frame.textBatch->vertices.push_back({toNdcX(qx1, fbW), toNdcY(qy1, fbH), z, abgr, tu1, tv1});
        frame.textBatch->vertices.push_back({toNdcX(qx0, fbW), toNdcY(qy1, fbH), z, abgr, tu0, tv1});
        frame.textBatch->indices.push_back(base + 0);
        frame.textBatch->indices.push_back(base + 1);
        frame.textBatch->indices.push_back(base + 2);
        frame.textBatch->indices.push_back(base + 0);
        frame.textBatch->indices.push_back(base + 2);
        frame.textBatch->indices.push_back(base + 3);
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
