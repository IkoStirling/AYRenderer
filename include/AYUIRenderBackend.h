#pragma once

#include "IAYRenderBackend.h"

#include <cstdint>
#include <memory>

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
    void drawWithAlpha(const ayt::math::FRectangle& bounds, void* textureHandle,
                       float alpha) override;

    TextMetrics measureText(const std::wstring& text, int fontSize,
                            float maxWidth = 0.0f) const override;
    ayt::font::FontMetrics getFontMetrics(ayt::font::FontHandle font) const override;
    ayt::font::FontHandle getFontHandle(const wchar_t* familyName, int baseSize) override;

    void flushBatches() override;

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

} // namespace ayt::render
