#pragma once

#include "AYUI/IRenderBackend.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace ayt::shader {
class ShaderResourcePool;
}

namespace ayt::font {
class IFont;
}

namespace ayt::render {

class Renderer;

namespace detail {
class BGFXAdapter;
class BgfxFontAtlas;
class UiGpuContext;
}

// bgfx-backed UI renderer for single-window editor composite (E2-composite).
// Mutable frame buffers live in a heap-allocated FrameState (see .cpp) so
// adding batching members does not change this class size — avoids MSVC stack
// cookie failures when EditorApp's uiBackend is stack-allocated and TUs are
// incrementally rebuilt with mismatched layouts.
class UIRenderBackend : public ayt::ui::IRenderBackend {
public:
    // Composite view map (ascending bgfx order):
    //   0 = full-window clear
    //   1 = ShadowPass caster → shadow FBO
    //   2 = ShadowPass resolve blit (color RT → sampleable tex)
    //   3 = ForwardOpaque / deferred composite hole → scene / panel
    //   6 = Skybox, 7 = GBuffer, 8 = Lighting, 9 = Transparent (Deferred)
    //  10 = BloomExtract (half-res bright)
    //  11 = BloomBlur horizontal, 12 = BloomBlur vertical
    //  13 = DepthHaze (half-res fog; before Final so same-frame sample)
    //  14 = PostProcess blit → backbuffer panel (Forward + Deferred)
    //  255 = UI chrome / menus (fixed high slot — insert Post passes
    //        without reshuffling UI; must stay > Final PP)
    static constexpr uint8_t kViewId = 255;

    UIRenderBackend();
    ~UIRenderBackend() override;

    UIRenderBackend(const UIRenderBackend&) = delete;
    UIRenderBackend& operator=(const UIRenderBackend&) = delete;

    // Our own drawRect overloads would otherwise hide the base
    // drawRect(bounds, BorderStyle) (C++ name lookup); bring it in so
    // UIRenderBackend-typed callers can use the BorderStyle form too.
    using ayt::ui::IRenderBackend::drawRect;

    bool initialize(Renderer& renderer);
    void shutdown();
    bool isInitialized() const { return _initialized; }

    void setFramebufferSize(uint16_t width, uint16_t height);

    void beginFrame() override;
    void endFrame() override;
    void beginCanvas(const ayt::math::FRectangle& viewport) override;
    void endCanvas() override;

    void drawRect(const ayt::math::FRectangle& bounds,
                  const ayt::math::FVector4& color) override;
    void drawRect(const ayt::math::FRectangle& bounds, void* textureHandle,
                  const ayt::math::FRectangle& uv) override;
    void drawText(const ayt::math::FRectangle& bounds, const std::wstring& text, int fontSize,
                  const ayt::math::FVector4& color) override;
    // Text-style drawText: implements Align + VAlign (outline/shadow/
    // letterSpacing/lineSpacing are documented as not-yet-implemented —
    // see .cpp). The simple overload routes through here with default
    // style (Left + Middle == legacy single-line behavior).
    void drawText(const ayt::math::FRectangle& bounds, const std::wstring& text, int fontSize,
                  const ayt::ui::IRenderBackend::TextStyle& style) override;
    void drawWithAlpha(const ayt::math::FRectangle& bounds, void* textureHandle,
                       float alpha) override;

    // P3: UI texture registry (non-virtual — AYUI only sees the interface,
    // which passes opaque handles into drawRect/drawWithAlpha/drawNinePatch).
    // Returns a fake-pointer handle, or nullptr on upload failure. Refcount
    // starts at 1; releaseUiTexture decrements and frees the GPU texture at
    // 0 (double-release is a safe no-op). The registry is persistent across
    // frames (beginFrame does not touch it) and fully released at shutdown.
    void* createUiTexture(uint16_t width, uint16_t height, const void* bgraPixels);
    void releaseUiTexture(void* textureHandle);

    // P1+P2 merged header edit (single header change to bound incremental
    // rebuilds): overrides that were previously inherited interface
    // defaults. setBlendMode / drawGradientRect land in P1; drawBorderRect /
    // drawRectShadow get SDF bodies in P2; drawNinePatch is a no-op until
    // P3's texture registry.
    void setBlendMode(ayt::ui::BlendMode mode) override;
    // PR-anim: stacked global opacity. Every color-emitting entry
    // (rect / gradient / SDF / text glyph) multiplies its alpha by the
    // stack top; Widget::render pushes per-node opacity so the tree
    // fades as a whole. Base frame is 1.0 (no-op).
    void pushOpacity(float alpha) override;
    void popOpacity() override;
    // IAYRenderBackend declares the 2-color (vertical) variant as
    // pure virtual — the 4-color override below does NOT satisfy it
    // (different signature). Implement both; the 2-color routes to the
    // 4-color path (top = both top corners, bottom = both bottom
    // corners), matching AYUI's MockRenderer + Test_UIGradientBlend
    // semantics.
    void drawGradientRect(const ayt::math::FRectangle& bounds,
                          const ayt::math::FVector4& topColor,
                          const ayt::math::FVector4& bottomColor) override;
    void drawGradientRect(const ayt::math::FRectangle& bounds,
                          const ayt::math::FVector4& topLeft,
                          const ayt::math::FVector4& topRight,
                          const ayt::math::FVector4& bottomLeft,
                          const ayt::math::FVector4& bottomRight) override;
    void drawBorderRect(const ayt::math::FRectangle& bounds, const ayt::math::FVector4& color,
                        float borderWidth, float cornerRadius = 0) override;
    void drawBorderRect(const ayt::math::FRectangle& bounds, const ayt::math::FVector4& color,
                        float borderWidth,
                        const ayt::ui::IRenderBackend::CornerRadii& radii) override;
    void drawRoundedRect(const ayt::math::FRectangle& bounds, const ayt::math::FVector4& color,
                         float cornerRadius = 0) override;
    void drawRoundedRect(const ayt::math::FRectangle& bounds, const ayt::math::FVector4& color,
                         const ayt::ui::IRenderBackend::CornerRadii& radii) override;
    void drawRectShadow(const ayt::math::FRectangle& bounds,
                        const ayt::ui::IRenderBackend::ShadowStyle& shadow) override;
    // Card: shadow + fill + stroke composited in ONE SDF submission (the
    // shader already blends the layers; this skips the three-draw API).
    void drawCard(const ayt::math::FRectangle& bounds,
                  const ayt::ui::IRenderBackend::CardStyle& style) override;
    void drawNinePatch(const ayt::math::FRectangle& bounds, void* textureHandle,
                       const ayt::math::FRectangle& uvRegion,
                       const ayt::math::FVector4& padding) override;

    TextMetrics measureText(const std::wstring& text, int fontSize,
                            float maxWidth = 0.0f) const override;
    ayt::font::FontMetrics getFontMetrics(ayt::font::FontHandle font) const override;
    ayt::font::FontHandle getFontHandle(const wchar_t* familyName, int baseSize) override;

    void flushBatches() override;

    void pushClip(const ayt::math::FRectangle& bounds) override;
    void popClip() override;

    int getDrawCallCount() const override { return _drawCalls; }

private:
    friend class Renderer;

    struct FrameState;

    bool initializeFromRenderer(Renderer& renderer, detail::BGFXAdapter& adapter,
                                shader::ShaderResourcePool& shaderPool);
    void shutdownFromRenderer(detail::BGFXAdapter& adapter,
                              shader::ShaderResourcePool& shaderPool);
    void shutdownFromRendererWithoutAdapter();

    void flushColoredRects();
    void flushPendingText();
    void syncTextAtlasIfNeeded();
    void drawTexturedQuad(const ayt::math::FRectangle& bounds, uint16_t textureIdx,
                          const ayt::math::FVector4& tint);
    // Textured-quad entry shared by drawRect(texture) / drawWithAlpha /
    // drawNinePatch: CPU-clips and remaps UVs by the clip fraction so a
    // partially-clipped quad crops instead of stretching (tint alpha rides
    // the opacity stack like every color-emitting entry).
    void emitClippedTexturedQuad(const ayt::math::FRectangle& bounds, uint16_t textureIdx,
                                 const ayt::math::FRectangle& uv,
                                 const ayt::math::FVector4& tint);
    ayt::math::FRectangle activeClipBounds() const;
    bool clipRect(ayt::math::FRectangle& inout) const;

    bool     _initialized = false;
    uint16_t _width         = 0;
    uint16_t _height        = 0;
    int      _drawCalls     = 0;

    detail::BGFXAdapter*                  _adapter     = nullptr;
    shader::ShaderResourcePool*           _shaderPool  = nullptr;
    std::unique_ptr<detail::UiGpuContext> _gpu;
    std::unique_ptr<detail::BgfxFontAtlas> _fontAtlas;
    std::unique_ptr<FrameState>           _frame;
};

// Text alignment math, shared by the styled drawText implementation and
// its unit tests (pure functions — no backend state). Horizontal: where a
// line of `lineWidth` px starts inside a bounds of `boundsWidth` px.
// Vertical: baseline Y for a line of `lineHeight` px inside `boundsHeight`
// px, `ascent` above the baseline. Middle is the legacy simple-drawText
// behavior (line centered in bounds, baseline = ascent below the top edge
// of that centered line box).
inline float uiTextAlignX(ayt::ui::IRenderBackend::TextStyle::Align align, float boundsMinX,
                          float boundsWidth, float lineWidth)
{
    // A line wider than the bounds clamps to the left edge (no negative
    // origin) — overflow degrades to left-aligned, matching common UI.
    switch (align) {
    case ayt::ui::IRenderBackend::TextStyle::Align::Center:
        return boundsMinX + std::max(0.0f, (boundsWidth - lineWidth) * 0.5f);
    case ayt::ui::IRenderBackend::TextStyle::Align::Right:
        return boundsMinX + std::max(0.0f, boundsWidth - lineWidth);
    case ayt::ui::IRenderBackend::TextStyle::Align::Left:
    default:
        return boundsMinX;
    }
}

inline float uiTextBaselineY(ayt::ui::IRenderBackend::TextStyle::VAlign valign, float boundsMinY,
                             float boundsHeight, float lineHeight, float ascent)
{
    // A line taller than the bounds clamps to the top edge (overflow
    // degrades to top-aligned).
    switch (valign) {
    case ayt::ui::IRenderBackend::TextStyle::VAlign::Top:
        return boundsMinY + ascent;
    case ayt::ui::IRenderBackend::TextStyle::VAlign::Bottom:
        return boundsMinY + std::max(0.0f, boundsHeight - lineHeight) + ascent;
    case ayt::ui::IRenderBackend::TextStyle::VAlign::Middle:
    default:
        return boundsMinY + std::max(0.0f, (boundsHeight - lineHeight) * 0.5f) + ascent;
    }
}

// Total width of `count` advances with `letterSpacing` px after each glyph.
// The trailing spacing (after the last glyph) is invisible but keeps wrap
// widths identical to the pen advance the draw path emits.
inline float uiTextLineWidth(const float* advances, int count, float letterSpacing)
{
    float w = 0.0f;
    for (int i = 0; i < count; ++i) {
        w += advances[i] + letterSpacing;
    }
    return w;
}

// One wrapped line: [begin,end) glyph range into the advance array plus
// its width (letterSpacing included) — used for per-line alignment.
struct UiTextLineRange {
    int   begin = 0;
    int   end   = 0;
    float width = 0.0f;
};

// Greedy word wrap over `count` advances constrained to maxWidth px — the
// same rules as measureText: break at the last space that fits (the space
// stays on the old line); a space-free run hard-breaks per glyph, so CJK
// degrades to per-glyph breaking naturally. isSpace[i] marks break
// candidates (std::vector<bool> by ref — bit-packed, no .data()). maxWidth
// <= 0 emits a single full line (no wrap).
inline void uiTextWrapToLines(const float* advances, const std::vector<bool>& isSpace, int count,
                              float maxWidth, float letterSpacing,
                              std::vector<UiTextLineRange>& outLines)
{
    outLines.clear();
    if (count <= 0) {
        return;
    }
    if (maxWidth <= 0.0f) {
        outLines.push_back({0, count, uiTextLineWidth(advances, count, letterSpacing)});
        return;
    }

    // Effective per-glyph width includes the trailing letterSpacing, so
    // prefix sums double as exact line widths for alignment.
    std::vector<float> prefix(static_cast<size_t>(count) + 1u, 0.0f);
    for (int i = 0; i < count; ++i) {
        prefix[static_cast<size_t>(i) + 1u] =
            prefix[static_cast<size_t>(i)] + advances[i] + letterSpacing;
    }

    int   lineStart = 0;
    int   lastBreak = -1;  // last space index in the current line; -1 = none
    float lineW     = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float w = advances[i] + letterSpacing;
        if (lineW + w <= maxWidth || lineW <= 0.0f) {
            lineW += w;
            if (isSpace[i]) {
                lastBreak = i;
            }
        } else {
            if (lastBreak >= lineStart) {
                // Word break: the space stays on the old line; the new line
                // starts with glyphs (lastBreak+1 .. i].
                outLines.push_back({lineStart, lastBreak + 1,
                                    prefix[static_cast<size_t>(lastBreak) + 1u]
                                        - prefix[static_cast<size_t>(lineStart)]});
                lineStart = lastBreak + 1;
                lineW     = prefix[static_cast<size_t>(i) + 1u]
                          - prefix[static_cast<size_t>(lineStart)];
                lastBreak = -1;
            } else {
                outLines.push_back({lineStart, i,
                                    prefix[static_cast<size_t>(i)]
                                        - prefix[static_cast<size_t>(lineStart)]});
                lineStart = i;
                lineW     = w;
            }
            if (isSpace[i]) {
                lastBreak = i;
            }
        }
    }
    if (lineStart < count) {
        outLines.push_back({lineStart, count,
                            prefix[static_cast<size_t>(count)]
                                - prefix[static_cast<size_t>(lineStart)]});
    }
}

} // namespace ayt::render
