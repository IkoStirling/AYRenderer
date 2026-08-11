// Test_UIGradientBlend.cpp — P1 gradients + blend modes on the unified
// batch: gradients are Flat items with per-corner colors (one run), blend
// modes are encoded per-item and break runs only when they differ.

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYUIRenderBackend.h"

#include <cstdio>

#if defined(_WIN32)
#  include <Windows.h>
#endif

namespace {

bool systemFontAvailable()
{
#if defined(_WIN32)
    const DWORD attr = GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf");
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return true;
    }
    const DWORD arial = GetFileAttributesW(L"C:\\Windows\\Fonts\\arial.ttf");
    return arial != INVALID_FILE_ATTRIBUTES && (arial & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return false;
#endif
}

struct Harness {
    ayt::render::Renderer renderer;
    ayt::render::UIRenderBackend ui;

    bool init()
    {
        ayt::render::InitDesc desc;
        desc.backend = ayt::render::Backend::Noop;
        desc.width   = 800;
        desc.height  = 600;
        if (!renderer.initialize(desc)) {
            return false;
        }
        if (!ui.initialize(renderer)) {
            renderer.shutdown();
            return false;
        }
        ui.setFramebufferSize(800, 600);
        return true;
    }

    ~Harness()
    {
        ui.shutdown();
        renderer.shutdown();
    }
};

void drawRectAt(ayt::render::UIRenderBackend& ui, float x, float y, float w, float h)
{
    ui.drawRect(ayt::math::FRectangle(x, y, x + w, y + h),
                ayt::math::FVector4(0.5f, 0.5f, 0.5f, 1.0f));
}

void drawGradientAt(ayt::render::UIRenderBackend& ui, float x, float y, float w, float h)
{
    ui.drawGradientRect(ayt::math::FRectangle(x, y, x + w, y + h),
                        ayt::math::FVector4(1.0f, 0.0f, 0.0f, 1.0f),   // TL
                        ayt::math::FVector4(0.0f, 1.0f, 0.0f, 1.0f),   // TR
                        ayt::math::FVector4(0.0f, 0.0f, 1.0f, 1.0f),   // BL
                        ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));  // BR
}

} // namespace

TEST_SUITE(UIGradientBlendTests)

TEST_CASE(ui_gradient_blend_five_gradients_one_run)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    for (int i = 0; i < 5; ++i) {
        drawGradientAt(h.ui, static_cast<float>(i) * 100.0f, 10.0f, 80.0f, 60.0f);
    }
    h.ui.endFrame();
    // Gradients are Flat items on the white texture with equal state: one run.
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_gradient_blend_two_color_routes_to_four)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    // 2-color overload routes through the interface inline default to the
    // 4-color override (top = both top corners, bottom = both bottom corners).
    h.ui.drawGradientRect(ayt::math::FRectangle(10.0f, 10.0f, 90.0f, 70.0f),
                          ayt::math::FVector4(0.1f, 0.2f, 0.3f, 1.0f),
                          ayt::math::FVector4(0.4f, 0.5f, 0.6f, 1.0f));
    drawRectAt(h.ui, 100.0f, 10.0f, 40.0f, 40.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_gradient_blend_mode_breaks_runs)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);            // Normal
    h.ui.setBlendMode(ayt::ui::BlendMode::Additive);
    drawRectAt(h.ui, 60.0f, 10.0f, 40.0f, 40.0f);            // Additive
    h.ui.setBlendMode(ayt::ui::BlendMode::Normal);
    drawRectAt(h.ui, 110.0f, 10.0f, 40.0f, 40.0f);           // Normal
    h.ui.endFrame();
    // Different states never merge — three consecutive runs.
    CHECK(h.ui.getDrawCallCount() == 3);
}

TEST_CASE(ui_gradient_blend_adjacent_same_mode_merges)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);
    drawRectAt(h.ui, 60.0f, 10.0f, 40.0f, 40.0f);
    h.ui.setBlendMode(ayt::ui::BlendMode::Additive);
    drawRectAt(h.ui, 110.0f, 10.0f, 40.0f, 40.0f);
    drawRectAt(h.ui, 160.0f, 10.0f, 40.0f, 40.0f);
    h.ui.setBlendMode(ayt::ui::BlendMode::Normal);
    drawRectAt(h.ui, 210.0f, 10.0f, 40.0f, 40.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 3);
}

TEST_CASE(ui_gradient_blend_mode_resets_each_frame)
{
    Harness h;
    CHECK(h.init());

    // Frame 1 leaves Additive as the trailing mode.
    h.ui.beginFrame();
    h.ui.setBlendMode(ayt::ui::BlendMode::Additive);
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    // Frame 2: if the mode leaked across frames the two rects would both be
    // Additive (one run); with the per-frame reset the second setBlendMode
    // splits them into Normal + Additive.
    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);   // must be Normal (reset)
    h.ui.setBlendMode(ayt::ui::BlendMode::Additive);
    drawRectAt(h.ui, 60.0f, 10.0f, 40.0f, 40.0f);   // Additive
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 2);
}

TEST_CASE(ui_gradient_blend_all_modes_one_run_each)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);   // Normal
    h.ui.setBlendMode(ayt::ui::BlendMode::Additive);
    drawGradientAt(h.ui, 60.0f, 10.0f, 80.0f, 60.0f);  // Additive gradient
    h.ui.setBlendMode(ayt::ui::BlendMode::Multiply);
    drawRectAt(h.ui, 150.0f, 10.0f, 40.0f, 40.0f);  // Multiply
    h.ui.setBlendMode(ayt::ui::BlendMode::Screen);
    drawRectAt(h.ui, 200.0f, 10.0f, 40.0f, 40.0f);  // Screen
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 4);
}

TEST_CASE(ui_gradient_blend_gradient_plus_text_two_runs)
{
    if (!systemFontAvailable()) {
        std::fprintf(stderr, "[UIGradientBlend text test] SKIP: system UI font not found\n");
        return;
    }
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawGradientAt(h.ui, 10.0f, 10.0f, 80.0f, 60.0f);
    h.ui.setBlendMode(ayt::ui::BlendMode::Additive);
    h.ui.drawText(ayt::math::FRectangle(100.0f, 10.0f, 200.0f, 36.0f), L"Play", 14,
                  ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    // gradient run (white) + glyph run (atlas): 2, glyph state Additive.
    CHECK(h.ui.getDrawCallCount() == 2);
}

TEST_SUITE_END
