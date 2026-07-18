#include "AYUIRenderBackend.h"

#include "AYRenderer.h"
#include "detail/BgfxFontAtlas.h"
#include "detail/BGFXAdapter.h"
#include "detail/UiGpuContext.h"
#include "AYShaderResourcePool.h"

#include <AYCoreUtility.h>

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

} // namespace

struct UIRenderBackend::FrameState {
    std::vector<ColoredRect>          pendingRects;
    std::vector<UiVertex>             scratchVertices;
    std::vector<uint32_t>             scratchIndices;
    std::unique_ptr<PendingTextBatch> textBatch;
    ayt::font::IFont*                 textSyncFont = nullptr;
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

    _gpu->beginView(kViewId, _width, _height);
}

void UIRenderBackend::beginCanvas(const ayt::math::FRectangle& viewport)
{
    AYUNREFERENCED_PARAM(viewport);
}

void UIRenderBackend::endCanvas() {}

void UIRenderBackend::endFrame()
{
    flushColoredRects();
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

    // Cut the text batch before any new rect so overlays drawn after text
    // keep correct z-order (rect submit follows flushed text).
    if (_frame->textBatch != nullptr && !_frame->textBatch->vertices.empty()) {
        flushPendingText();
    }

    _frame->pendingRects.push_back({bounds, color});
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

void UIRenderBackend::drawText(const ayt::math::FRectangle& bounds, const std::wstring& text,
                               int fontSize, const ayt::math::FVector4& color)
{
    if (!_initialized || text.empty() || _width < 1 || _height < 1 || _gpu == nullptr
        || _adapter == nullptr || _fontAtlas == nullptr) {
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

    _fontAtlas->prepareGlyphs(font, fontSize, text);
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

    for (wchar_t ch : text) {
        ayt::font::GlyphInfo* glyph = font->getGlyph(static_cast<uint32_t>(ch));
        if (glyph == nullptr) {
            cursorX += metrics.lineHeight * 0.25f;
            continue;
        }

        const int glyphW = glyph->metrics.width;
        const int glyphH = glyph->metrics.height;
        if (glyphW <= 0 || glyphH <= 0) {
            cursorX += static_cast<float>(glyph->metrics.advance);
            continue;
        }

        const float x0 = cursorX + static_cast<float>(glyph->metrics.bearingX);
        const float y0 = baselineY - static_cast<float>(glyph->metrics.bearingY);
        const float x1 = x0 + static_cast<float>(glyphW);
        const float y1 = y0 + static_cast<float>(glyphH);

        const float u0 = glyph->atlasPosX / atlasW;
        const float v0 = glyph->atlasPosY / atlasH;
        const float u1 = (glyph->atlasPosX + glyph->atlasWidth) / atlasW;
        const float v1 = (glyph->atlasPosY + glyph->atlasHeight) / atlasH;

        const uint32_t base = static_cast<uint32_t>(frame.textBatch->vertices.size());
        const float    z    = 0.0f;
        frame.textBatch->vertices.push_back({toNdcX(x0, fbW), toNdcY(y0, fbH), z, abgr, u0, v0});
        frame.textBatch->vertices.push_back({toNdcX(x1, fbW), toNdcY(y0, fbH), z, abgr, u1, v0});
        frame.textBatch->vertices.push_back({toNdcX(x1, fbW), toNdcY(y1, fbH), z, abgr, u1, v1});
        frame.textBatch->vertices.push_back({toNdcX(x0, fbW), toNdcY(y1, fbH), z, abgr, u0, v1});
        frame.textBatch->indices.push_back(base + 0);
        frame.textBatch->indices.push_back(base + 1);
        frame.textBatch->indices.push_back(base + 2);
        frame.textBatch->indices.push_back(base + 0);
        frame.textBatch->indices.push_back(base + 2);
        frame.textBatch->indices.push_back(base + 3);
        cursorX += static_cast<float>(glyph->metrics.advance);
    }
}

} // namespace ayt::render
