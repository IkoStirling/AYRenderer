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

UIRenderBackend::UIRenderBackend() = default;

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
    _pendingRects.clear();
}

void UIRenderBackend::shutdownFromRendererWithoutAdapter()
{
    _gpu.reset();
    _fontAtlas.reset();
    _adapter     = nullptr;
    _shaderPool  = nullptr;
    _initialized = false;
    _pendingRects.clear();
}

void UIRenderBackend::setFramebufferSize(uint16_t width, uint16_t height)
{
    _width  = width;
    _height = height;
}

void UIRenderBackend::beginFrame()
{
    _drawCalls = 0;
    _pendingRects.clear();

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

void UIRenderBackend::drawRect(const ayt::math::FRectangle& bounds, const ayt::math::FVector4& color)
{
    _pendingRects.push_back({bounds, color});
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
    if (_pendingRects.empty() || !_initialized || _gpu == nullptr || _adapter == nullptr
        || _width < 1 || _height < 1) {
        _pendingRects.clear();
        return;
    }

    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(_pendingRects.size() * 4);
    indices.reserve(_pendingRects.size() * 6);

    for (const ColoredRect& rect : _pendingRects) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        const uint32_t abgr = toAbgr(rect.color);
        const float    z    = 0.0f;

        vertices.push_back({toNdcX(rect.bounds.minX, static_cast<float>(_width)),
                            toNdcY(rect.bounds.minY, static_cast<float>(_height)), z, abgr, 0.0f,
                            0.0f});
        vertices.push_back({toNdcX(rect.bounds.maxX, static_cast<float>(_width)),
                            toNdcY(rect.bounds.minY, static_cast<float>(_height)), z, abgr, 1.0f,
                            0.0f});
        vertices.push_back({toNdcX(rect.bounds.maxX, static_cast<float>(_width)),
                            toNdcY(rect.bounds.maxY, static_cast<float>(_height)), z, abgr, 1.0f,
                            1.0f});
        vertices.push_back({toNdcX(rect.bounds.minX, static_cast<float>(_width)),
                            toNdcY(rect.bounds.maxY, static_cast<float>(_height)), z, abgr, 0.0f,
                            1.0f});

        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }

    _gpu->submitColoredQuads(kViewId, *_adapter, vertices.data(),
                             static_cast<uint32_t>(vertices.size()), sizeof(UiVertex),
                             indices.data(), static_cast<uint32_t>(indices.size()));

    ++_drawCalls;
    _pendingRects.clear();
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
    if (text.empty() || _width < 1 || _height < 1 || _gpu == nullptr || _adapter == nullptr
        || _fontAtlas == nullptr) {
        return;
    }

    ayt::font::IFont* font = _fontAtlas->acquireFont(fontSize);
    if (font == nullptr) {
        drawRect(bounds, color);
        return;
    }

    for (wchar_t ch : text) {
        font->getGlyph(static_cast<uint32_t>(ch));
    }

    _fontAtlas->markAtlasDirty();
    flushColoredRects();
    _fontAtlas->syncAtlasToGpu(font);

    const uint16_t atlasIdx = _fontAtlas->atlasTextureIdx();
    if (atlasIdx == detail::UiGpuContext::kInvalidIdx) {
        return;
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

    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(text.size() * 4u);
    indices.reserve(text.size() * 6u);

    auto appendGlyphQuad = [&](float x0, float y0, float x1, float y1, float u0, float v0,
                               float u1, float v1) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        const float    z    = 0.0f;
        vertices.push_back({toNdcX(x0, fbW), toNdcY(y0, fbH), z, abgr, u0, v0});
        vertices.push_back({toNdcX(x1, fbW), toNdcY(y0, fbH), z, abgr, u1, v0});
        vertices.push_back({toNdcX(x1, fbW), toNdcY(y1, fbH), z, abgr, u1, v1});
        vertices.push_back({toNdcX(x0, fbW), toNdcY(y1, fbH), z, abgr, u0, v1});
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    };

    for (wchar_t ch : text) {
        ayt::font::GlyphInfo* glyph = font->getGlyph(static_cast<uint32_t>(ch));
        if (glyph == nullptr) {
            cursorX += metrics.lineHeight * 0.25f;
            continue;
        }

        if (glyph->metrics.width <= 0 || glyph->metrics.height <= 0) {
            cursorX += static_cast<float>(glyph->metrics.advance);
            continue;
        }

        const float x0 = cursorX + static_cast<float>(glyph->metrics.bearingX);
        const float y0 = baselineY - static_cast<float>(glyph->metrics.bearingY);
        const float x1 = x0 + static_cast<float>(glyph->metrics.width);
        const float y1 = y0 + static_cast<float>(glyph->metrics.height);

        const float u0 = glyph->atlasPosX / atlasW;
        const float v0 = glyph->atlasPosY / atlasH;
        const float u1 = (glyph->atlasPosX + glyph->atlasWidth) / atlasW;
        const float v1 = (glyph->atlasPosY + glyph->atlasHeight) / atlasH;

        appendGlyphQuad(x0, y0, x1, y1, u0, v0, u1, v1);
        cursorX += static_cast<float>(glyph->metrics.advance);
    }

    if (vertices.empty()) {
        return;
    }

    _gpu->submitTexturedQuads(kViewId, *_adapter, atlasIdx, vertices.data(),
                              static_cast<uint32_t>(vertices.size()), sizeof(UiVertex),
                              indices.data(), static_cast<uint32_t>(indices.size()));
    ++_drawCalls;
}

} // namespace ayt::render
