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
class RenderTargetPool;
class UiGpuContext;
}

// bgfx-backed UI renderer for single-window editor composite (E2-composite).
// Mutable frame buffers live in a heap-allocated FrameState (see .cpp) so
// adding batching members does not change this class size — avoids MSVC stack
// cookie failures when EditorApp's uiBackend is stack-allocated and TUs are
// incrementally rebuilt with mismatched layouts.
class UIRenderBackend : public ayt::ui::IRenderBackend {
public:
    // OrderedRuns is the original conservative implementation: only
    // immediately adjacent compatible items merge. OverlapAware keeps the
    // same painter-order result while allowing non-overlapping items to move
    // across incompatible neighbours and join a larger material batch.
    enum class BatchMode : uint8_t {
        OverlapAware,
        OrderedRuns
    };

    // Composite view map (dependency order is explicitly remapped):
    //   0 = full-window clear
    //   1 = ShadowPass caster → shadow FBO
    //   2 = ShadowPass resolve blit (color RT → sampleable tex)
    //   3 = ForwardOpaque / deferred composite hole → scene / panel
    //   6 = Skybox, 7 = GBuffer, 8 = Lighting, 9 = Transparent (Deferred)
    //  10 = BloomExtract (half-res bright)
    //  11 = BloomBlur horizontal, 12 = BloomBlur vertical
    //  13 = DepthHaze, 14 = SSAO (ordered before Lighting)
    //  15 = PostProcess → FrameGraph FinalLdrColor
    //  16 = Present → backbuffer panel (Forward + Deferred)
    //  17 = FXAA offscreen LDR filter (ordered before Present)
    //  18–25 = Shadow atlas slots
    //  26–243 = retained UI Layer / generic offscreen paint targets
    //  244 = unjittered selection silhouette; 245 = TAA diagnostics
    //  246 = camera-overlay 2D composition
    //  247–249 = SMAA 1x edge / blend-weight / neighborhood stages
    //  250 = GBufferDebug, 251/252 = EditorOverlay axis/compatibility
    //  253/254 = jittered selection visibility / screen-space composite
    //  255 = UI chrome / menus (fixed high slot — insert Post passes
    //        without reshuffling UI; must stay after presentation overlays)
    static constexpr uint8_t kViewId = 255;
    static constexpr uint8_t kFirstLayerViewId = 26;
    static constexpr uint8_t kLastLayerViewId = 243;
    static constexpr uint16_t kMaxOffscreenPaintsPerFrame =
        static_cast<uint16_t>(kLastLayerViewId - kFirstLayerViewId + 1u);

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

    bool supportsRenderTargets() const override;
    RenderTargetHandle createRenderTarget(int width, int height,
                                          bool hasAlpha = true) override;
    RenderTargetHandle createRenderTarget(const RenderTargetDesc& desc) override;
    bool resizeRenderTarget(RenderTargetHandle target,
                            const RenderTargetDesc& desc) override;
    void releaseRenderTarget(RenderTargetHandle target) override;
    void bindRenderTarget(RenderTargetHandle target) override;
    void* getRenderTargetTexture(RenderTargetHandle target) override;
    void blitRenderTarget(RenderTargetHandle source,
                          const ayt::math::FRectangle& destBounds) override;

    LayerHandle createLayer(const LayerDesc& desc) override;
    void releaseLayer(LayerHandle layer) override;
    bool updateLayer(LayerHandle layer, const LayerDesc& desc) override;
    bool beginLayerPaint(LayerHandle layer, const LayerPaint& paint) override;
    void endLayerPaint(LayerHandle layer) override;
    void compositeLayer(LayerHandle layer,
                        const ayt::math::FRectangle& destBounds,
                        float opacity = 1.0f) override;
    void invalidateLayer(LayerHandle layer,
                         const ayt::math::FRectangle& damage =
                             ayt::math::FRectangle()) override;
    bool isLayerDirty(LayerHandle layer) const override;
    LayerCacheStats getLayerCacheStats() const override;
    void setLayerCacheBudgetBytes(size_t bytes) override;
    void resetLayerCacheStats() override;

    void setFramebufferSize(uint16_t width, uint16_t height);

    // OverlapAware is the default. OrderedRuns remains available as a
    // runtime safety fallback and for A/B diagnostics.
    void setBatchMode(BatchMode mode);
    BatchMode getBatchMode() const;

    void beginFrame() override;
    void endFrame() override;
    void setUiScale(float scale) override;
    float getUiScale() const override;
    void beginCanvas(const ayt::math::FRectangle& viewport) override;
    void endCanvas() override;

    void drawRect(const ayt::math::FRectangle& bounds,
                  const ayt::math::FVector4& color) override;
    void drawRect(const ayt::math::FRectangle& bounds, void* textureHandle,
                  const ayt::math::FRectangle& uv) override;
    bool addTexturedQuad(const ayt::math::FRectangle& bounds,
                         void* textureHandle,
                         const ayt::math::FRectangle& uv,
                         const ayt::math::FVector4& tint) override;
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
    // The UI backend supports the axis-aligned uniform scale + translation
    // subset used by retained editor viewports. Transform boundaries flush
    // the current batch, preserving ordering while keeping the normal fast
    // path unchanged for trees that do not use a transform.
    void pushTransform(const ayt::math::Float4x4& transform) override;
    void popTransform() override;
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
    TextMetrics measureText(const std::wstring& text, int fontSize,
                            const ayt::ui::IRenderBackend::TextStyle& style,
                            float maxWidth = 0.0f) const override;
    ShapedText shapeText(const std::wstring& text, int fontSize,
                         const ayt::ui::IRenderBackend::TextStyle& style) const override;
    ayt::font::FontMetrics getFontMetrics(ayt::font::FontHandle font) const override;
    ayt::font::FontHandle getFontHandle(const wchar_t* familyName, int baseSize) override;

    void flushBatches() override;

    void pushClip(const ayt::math::FRectangle& bounds) override;
    void popClip() override;

    // CPU-authored vector paths are tessellated into triangle meshes and
    // submitted through the same UI shader/batch stream. Path clips use the
    // framebuffer stencil plane and therefore support nesting and explicit
    // clockwise hole contours.
    PathHandle createPath() override;
    void releasePath(PathHandle path) override;
    void addPathRect(PathHandle path, const ayt::math::FRectangle& bounds,
                     PathWinding winding = PathWinding::CounterClockwise) override;
    void addPathRoundedRect(PathHandle path, const ayt::math::FRectangle& bounds,
                            float cornerRadius,
                            PathWinding winding = PathWinding::CounterClockwise) override;
    void addPathEllipse(PathHandle path, const ayt::math::FVector2& center,
                        float radiusX, float radiusY,
                        PathWinding winding = PathWinding::CounterClockwise) override;
    void addPathLine(PathHandle path, const ayt::math::FVector2& start,
                     const ayt::math::FVector2& end) override;
    void addPathBezier(PathHandle path, const ayt::math::FVector2& start,
                       const ayt::math::FVector2& control1,
                       const ayt::math::FVector2& control2,
                       const ayt::math::FVector2& end) override;
    void addPathArc(PathHandle path, const ayt::math::FVector2& center,
                    float radius, float startAngle, float endAngle,
                    PathWinding winding = PathWinding::CounterClockwise) override;
    void addPathPolygon(PathHandle path, const ayt::math::FVector2* points,
                        int count,
                        PathWinding winding = PathWinding::CounterClockwise) override;
    void addPathContour(PathHandle path, const ayt::math::FVector2* points,
                        int count, bool closed,
                        PathWinding winding = PathWinding::CounterClockwise) override;
    void setPathFillColor(PathHandle path, const ayt::math::FVector4& color) override;
    void setPathStrokeColor(PathHandle path, const ayt::math::FVector4& color) override;
    void setPathStrokeWidth(PathHandle path, float width) override;
    void setPathStrokeStyle(PathHandle path, PathStrokeCap cap,
                            PathStrokeJoin join,
                            float miterLimit = 4.0f) override;
    void drawPath(PathHandle path,
                  PathFillMode mode = PathFillMode::Fill) override;
    void pushPathClip(PathHandle path) override;

    struct PathDebugInfo {
        size_t contourCount = 0;
        size_t clockwiseContourCount = 0;
        size_t openContourCount = 0;
        size_t fillTriangleCount = 0;
        size_t strokeTriangleCount = 0;
        ayt::math::FRectangle bounds;
    };
    bool getPathDebugInfo(PathHandle path, PathDebugInfo& outInfo) const;
    uint8_t getActivePathClipDepthForDebug() const;
    size_t getPendingUiItemCountForDebug() const;
    bool hasPathOrderingBarrierForDebug() const;

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
    void syncTextAtlasIfNeeded(ayt::font::IFont* font);
    void drawTexturedQuad(const ayt::math::FRectangle& bounds, uint16_t textureIdx,
                          const ayt::math::FVector4& tint);
    // Textured-quad entry shared by drawRect(texture) / drawWithAlpha /
    // drawNinePatch: CPU-clips and remaps UVs by the clip fraction so a
    // partially-clipped quad crops instead of stretching (tint alpha rides
    // the opacity stack like every color-emitting entry).
    void emitClippedTexturedQuad(const ayt::math::FRectangle& bounds, uint16_t textureIdx,
                                 const ayt::math::FRectangle& uv,
                                 const ayt::math::FVector4& tint,
                                 uint64_t stateOverride = 0);
    bool resolveTextureHandle(void* handle, uint16_t& textureIdx,
                              uint16_t& width, uint16_t& height) const;
    bool ensureRenderTarget(int targetId);
    bool releaseLruLayerBacking(int excludeTargetId);
    ayt::math::FRectangle activeClipBounds() const;
    bool clipRect(ayt::math::FRectangle& inout) const;

    bool     _initialized = false;
    uint16_t _width         = 0;
    uint16_t _height        = 0;
    int      _drawCalls     = 0;

    detail::BGFXAdapter*                  _adapter     = nullptr;
    detail::RenderTargetPool*             _targetPool  = nullptr;
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
