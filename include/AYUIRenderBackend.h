#pragma once

#include "AYIRenderBackend.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ayt::shader {
class ShaderResourcePool;
}

namespace ayt::render {

class Renderer;

namespace detail {
class BGFXAdapter;
class BgfxFontAtlas;
class UiGpuContext;
}

// bgfx-backed UI renderer for single-window editor composite (E2-composite).
class UIRenderBackend : public ayt::ui::IRenderBackend {
public:
    static constexpr uint8_t kViewId = 1;

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

    int getDrawCallCount() const override { return _drawCalls; }

private:
    friend class Renderer;

    bool initializeFromRenderer(Renderer& renderer, detail::BGFXAdapter& adapter,
                                shader::ShaderResourcePool& shaderPool);
    void shutdownFromRenderer(detail::BGFXAdapter& adapter,
                              shader::ShaderResourcePool& shaderPool);
    void shutdownFromRendererWithoutAdapter();

    void flushColoredRects();
    void drawTexturedQuad(const ayt::math::FRectangle& bounds, uint16_t textureIdx,
                          const ayt::math::FVector4& tint);

    struct ColoredRect {
        ayt::math::FRectangle bounds;
        ayt::math::FVector4   color;
    };

    struct UiVertex {
        float    x;
        float    y;
        float    z;
        uint32_t abgr;
        float    u;
        float    v;
    };

    bool     _initialized = false;
    uint16_t _width         = 0;
    uint16_t _height        = 0;
    int      _drawCalls     = 0;

    detail::BGFXAdapter*                  _adapter     = nullptr;
    shader::ShaderResourcePool*           _shaderPool  = nullptr;
    std::unique_ptr<detail::UiGpuContext> _gpu;
    std::unique_ptr<detail::BgfxFontAtlas> _fontAtlas;

    std::vector<ColoredRect> _pendingRects;
};

} // namespace ayt::render
