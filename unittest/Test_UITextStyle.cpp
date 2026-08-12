// Test_UITextStyle.cpp — styled drawText (Align + VAlign) + measureText
// wrapping. Alignment math is exercised through the pure functions
// (uiTextAlignX / uiTextBaselineY); dispatch is asserted via draw calls
// on the Noop backend (shaders don't execute, geometry can't be read).

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYUIRenderBackend.h"

#include <cstdio>
#include <vector>

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
    // FRectangle is (minX, minY, maxX, maxY) — a 400x130 box at (100,100).
    h.ui.drawText(ayt::math::FRectangle(100, 100, 500, 230), L"Hello", 16,
                  ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.beginFrame();
    ayt::ui::IRenderBackend::TextStyle style;
    style.color  = ayt::math::FVector4(0.9f, 0.1f, 0.1f, 1.0f);
    style.align  = ayt::ui::IRenderBackend::TextStyle::Align::Center;
    style.valign = ayt::ui::IRenderBackend::TextStyle::VAlign::Top;
    h.ui.drawText(ayt::math::FRectangle(100, 100, 500, 230), L"Centered", 16, style);
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

TEST_CASE(ui_text_line_width_letter_spacing_pure)
{
    const float adv[3] = {10.0f, 20.0f, 15.0f};
    CHECK(ayt::render::uiTextLineWidth(adv, 3, 0.0f) == 45.0f);
    // letterSpacing applies after every glyph (trailing spacing invisible
    // but keeps wrap widths == pen advances): 10+4+20+4+15+4 = 57.
    CHECK(ayt::render::uiTextLineWidth(adv, 3, 4.0f) == 57.0f);
    CHECK(ayt::render::uiTextLineWidth(adv, 0, 4.0f) == 0.0f);
}

TEST_CASE(ui_text_wrap_to_lines_pure_word_break)
{
    // "AA BB" → advances [10,10,6,10,10], space at 2, maxWidth 30.
    const float adv[5] = {10.0f, 10.0f, 6.0f, 10.0f, 10.0f};
    const std::vector<bool> spc = {false, false, true, false, false};
    std::vector<ayt::render::UiTextLineRange> lines;
    ayt::render::uiTextWrapToLines(adv, spc, 5, 30.0f, 0.0f, lines);
    CHECK(lines.size() == 2);
    // The space stays on line 1; line 2 starts after it.
    CHECK(lines[0].begin == 0);
    CHECK(lines[0].end == 3);
    CHECK(lines[0].width == 26.0f);
    CHECK(lines[1].begin == 3);
    CHECK(lines[1].end == 5);
    CHECK(lines[1].width == 20.0f);
}

TEST_CASE(ui_text_wrap_to_lines_pure_hard_break)
{
    // No spaces → per-glyph breaking (CJK degradation path).
    const float adv[4] = {10.0f, 10.0f, 10.0f, 10.0f};
    const std::vector<bool> spc(4, false);
    std::vector<ayt::render::UiTextLineRange> lines;
    ayt::render::uiTextWrapToLines(adv, spc, 4, 25.0f, 0.0f, lines);
    CHECK(lines.size() == 2);
    CHECK(lines[0].begin == 0);
    CHECK(lines[0].end == 2);
    CHECK(lines[1].begin == 2);
    CHECK(lines[1].end == 4);
}

TEST_CASE(ui_text_wrap_to_lines_pure_letter_spacing)
{
    // Effective widths [15,15] (10 + ls 5) → both break.
    const float adv[2] = {10.0f, 10.0f};
    const std::vector<bool> spc(2, false);
    std::vector<ayt::render::UiTextLineRange> lines;
    ayt::render::uiTextWrapToLines(adv, spc, 2, 22.0f, 5.0f, lines);
    CHECK(lines.size() == 2);
    CHECK(lines[0].width == 15.0f);
    CHECK(lines[1].width == 15.0f);
}

TEST_CASE(ui_text_wrap_to_lines_pure_no_max_single_line)
{
    const float adv[3] = {10.0f, 20.0f, 15.0f};
    const std::vector<bool> spc = {false, true, false};
    std::vector<ayt::render::UiTextLineRange> lines;
    ayt::render::uiTextWrapToLines(adv, spc, 3, 0.0f, 0.0f, lines);
    CHECK(lines.size() == 1);
    CHECK(lines[0].begin == 0);
    CHECK(lines[0].end == 3);
    CHECK(lines[0].width == 45.0f);
}

TEST_CASE(ui_text_styled_outline_shadow_letter_spacing_dispatch)
{
    if (!systemFontAvailable()) {
        return;
    }
    TextHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    ayt::ui::IRenderBackend::TextStyle style;
    style.color          = ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    style.outlineColor   = ayt::math::FVector4(0.0f, 0.0f, 0.0f, 1.0f);
    style.outlineWidth   = 2.0f;
    style.shadowColor    = ayt::math::FVector4(0.0f, 0.0f, 0.0f, 0.5f);
    style.shadowOffset   = ayt::math::FVector2(3.0f, 3.0f);
    style.letterSpacing  = 2;
    h.ui.drawText(ayt::math::FRectangle(100, 100, 600, 230), L"Outlined Shadowed", 16, style);
    h.ui.endFrame();
    // shadow(1) + outline(4-dir) + fill(1) = 6 passes over the same
    // atlas/state → one flush run.
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_text_styled_wrap_to_bounds_dispatch)
{
    if (!systemFontAvailable()) {
        return;
    }
    TextHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    ayt::ui::IRenderBackend::TextStyle style;
    style.wrapToBounds = true;
    style.lineSpacing  = 4;
    style.valign       = ayt::ui::IRenderBackend::TextStyle::VAlign::Middle;
    // 3 words in a 120-wide box → wraps to multiple lines; block layout
    // with lineSpacing + block-centered VAlign must not crash and must
    // stay one run.
    h.ui.drawText(ayt::math::FRectangle(100, 100, 220, 300), L"AAAA BBBB CCCC", 16, style);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_text_styled_wrap_disabled_single_line)
{
    if (!systemFontAvailable()) {
        return;
    }
    TextHarness h;
    CHECK(h.init());

    // Default (wrapToBounds false): wider-than-bounds text stays one
    // clipped line — the legacy behavior must not regress.
    h.ui.beginFrame();
    ayt::ui::IRenderBackend::TextStyle style;
    style.color = ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    h.ui.drawText(ayt::math::FRectangle(100, 100, 160, 230), L"OverflowClippedText", 16, style);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

} // TEST_SUITE
