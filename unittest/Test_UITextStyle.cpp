// Test_UITextStyle.cpp — styled drawText (Align + VAlign) + measureText
// wrapping. Alignment math is exercised through the pure functions
// (uiTextAlignX / uiTextBaselineY); dispatch is asserted via draw calls
// on the Noop backend (shaders don't execute, geometry can't be read).

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

struct TextHarness {
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

    ~TextHarness()
    {
        ui.shutdown();
        renderer.shutdown();
    }
};

} // namespace

TEST_SUITE(UITextStyleTests)

TEST_CASE(ui_text_align_x_pure)
{
    using Align = ayt::ui::IRenderBackend::TextStyle::Align;
    // bounds = 100..300 (200 wide); line 60 px wide.
    CHECK(ayt::render::uiTextAlignX(Align::Left, 100.0f, 200.0f, 60.0f) == 100.0f);
    CHECK(ayt::render::uiTextAlignX(Align::Center, 100.0f, 200.0f, 60.0f) == 170.0f);
    CHECK(ayt::render::uiTextAlignX(Align::Right, 100.0f, 200.0f, 60.0f) == 240.0f);
    // Line wider than bounds clamps to the left edge.
    CHECK(ayt::render::uiTextAlignX(Align::Center, 100.0f, 200.0f, 240.0f) == 100.0f);
    CHECK(ayt::render::uiTextAlignX(Align::Right, 100.0f, 200.0f, 240.0f) == 100.0f);
}

TEST_CASE(ui_text_baseline_y_pure)
{
    using VAlign = ayt::ui::IRenderBackend::TextStyle::VAlign;
    // bounds 200..300 (100 tall), lineHeight 20, ascent 14.
    // Top: baseline = top edge + ascent.
    CHECK(ayt::render::uiTextBaselineY(VAlign::Top, 200.0f, 100.0f, 20.0f, 14.0f) == 214.0f);
    // Middle: line box centered in bounds.
    CHECK(ayt::render::uiTextBaselineY(VAlign::Middle, 200.0f, 100.0f, 20.0f, 14.0f) == 254.0f);
    // Bottom: line box flush to the bottom edge.
    CHECK(ayt::render::uiTextBaselineY(VAlign::Bottom, 200.0f, 100.0f, 20.0f, 14.0f) == 294.0f);
    // Line taller than bounds clamps to the top edge (all three agree).
    CHECK(ayt::render::uiTextBaselineY(VAlign::Top, 200.0f, 100.0f, 120.0f, 14.0f) == 214.0f);
    CHECK(ayt::render::uiTextBaselineY(VAlign::Middle, 200.0f, 100.0f, 120.0f, 14.0f) == 214.0f);
    CHECK(ayt::render::uiTextBaselineY(VAlign::Bottom, 200.0f, 100.0f, 120.0f, 14.0f) == 214.0f);
}

TEST_CASE(ui_text_styled_route_and_dispatch)
{
    TextHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawText(ayt::math::FRectangle(100, 100, 400, 130), L"Hello", 16,
                  ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.beginFrame();
    ayt::ui::IRenderBackend::TextStyle style;
    style.color  = ayt::math::FVector4(0.9f, 0.1f, 0.1f, 1.0f);
    style.align  = ayt::ui::IRenderBackend::TextStyle::Align::Center;
    style.valign = ayt::ui::IRenderBackend::TextStyle::VAlign::Top;
    h.ui.drawText(ayt::math::FRectangle(100, 100, 400, 130), L"Centered", 16, style);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_text_measure_wrap_single_line_no_max)
{
    if (!systemFontAvailable()) {
        return;
    }
    TextHarness h;
    CHECK(h.init());

    const auto m = h.ui.measureText(L"Hello World", 16);
    CHECK(m.width > 0.0f);
    CHECK(m.height > 0.0f);
    CHECK(m.ascent > 0.0f);
    // No maxWidth: single line — height is exactly one lineHeight.
    const auto one = h.ui.measureText(L"A", 16);
    CHECK(m.height == one.height);
}

TEST_CASE(ui_text_measure_wrap_greedy_word)
{
    if (!systemFontAvailable()) {
        return;
    }
    TextHarness h;
    CHECK(h.init());

    // Two 4-char words. Wrap at ~1 word per line → exactly 2 lines.
    const float full = h.ui.measureText(L"AAAA BBBB", 16).width;
    const float word = h.ui.measureText(L"AAAA", 16).width;
    const float oneH = h.ui.measureText(L"A", 16).height;
    const float maxW = word * 1.05f;
    const auto  m    = h.ui.measureText(L"AAAA BBBB", 16, maxW);
    CHECK(m.height == oneH * 2.0f);
    CHECK(m.width <= maxW);
    CHECK(m.width > 0.0f);
    CHECK(full > maxW);  // sanity: the constraint actually binds
}

TEST_CASE(ui_text_measure_wrap_hard_break_no_spaces)
{
    if (!systemFontAvailable()) {
        return;
    }
    TextHarness h;
    CHECK(h.init());

    // 8 identical glyphs, no spaces: word wrap degrades to per-glyph
    // hard breaking. maxWidth ≈ 2.5 glyph widths → 2 per line → 4 lines.
    const float oneA  = h.ui.measureText(L"A", 16).width;
    const float oneH  = h.ui.measureText(L"A", 16).height;
    const auto  m     = h.ui.measureText(L"AAAAAAAA", 16, oneA * 2.5f);
    CHECK(m.height == oneH * 4.0f);
    CHECK(m.width <= oneA * 2.5f);
}

TEST_CASE(ui_text_measure_wrap_max_wide_enough_stays_one_line)
{
    if (!systemFontAvailable()) {
        return;
    }
    TextHarness h;
    CHECK(h.init());

    const float full = h.ui.measureText(L"Hello World", 16).width;
    const float oneH = h.ui.measureText(L"A", 16).height;
    const auto  m    = h.ui.measureText(L"Hello World", 16, full * 1.1f);
    CHECK(m.height == oneH);
    CHECK(m.width == full);
}

} // TEST_SUITE
