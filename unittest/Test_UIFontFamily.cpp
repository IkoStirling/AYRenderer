// Test_UIFontFamily.cpp — getFontHandle(familyName, baseSize) honors the
// family dimension: known families resolve to their own face (lazily
// registered per size), unknown/empty names fall back to the size-keyed
// default face (legacy behavior).

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYRenderer/UIRenderBackend.h"

#include <cmath>
#include <cstdio>

#if defined(_WIN32)
#  include <Windows.h>
#endif

namespace {

struct FontHarness {
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

    ~FontHarness()
    {
        ui.shutdown();
        renderer.shutdown();
    }
};

bool msyhAvailable()
{
#if defined(_WIN32)
    const DWORD attr = GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc");
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return false;
#endif
}

bool segoeAvailable()
{
#if defined(_WIN32)
    const DWORD attr = GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf");
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return false;
#endif
}

} // namespace

TEST_SUITE(UIFontFamilyTests)

TEST_CASE(ui_font_family_known_resolves)
{
    if (!msyhAvailable()) {
        return;
    }
    FontHarness h;
    CHECK(h.init());

    const ayt::font::FontHandle handle = h.ui.getFontHandle(L"Microsoft YaHei", 16);
    CHECK(handle.isValid());
    const ayt::font::FontMetrics fm = h.ui.getFontMetrics(handle);
    CHECK(fm.ascent > 0.0f);
    CHECK(fm.lineHeight > 0.0f);
}

TEST_CASE(ui_font_family_unknown_falls_back_to_default)
{
    FontHarness h;
    CHECK(h.init());

    const ayt::font::FontHandle handle = h.ui.getFontHandle(L"Bogus Family Not Installed", 16);
    CHECK(handle.isValid());
}

TEST_CASE(ui_font_family_null_falls_back_to_default)
{
    FontHarness h;
    CHECK(h.init());

    const ayt::font::FontHandle handle = h.ui.getFontHandle(nullptr, 16);
    CHECK(handle.isValid());
}

TEST_CASE(ui_font_family_acquire_stable_handle)
{
    if (!msyhAvailable()) {
        return;
    }
    FontHarness h;
    CHECK(h.init());

    const ayt::font::FontHandle a = h.ui.getFontHandle(L"Microsoft YaHei", 18);
    const ayt::font::FontHandle b = h.ui.getFontHandle(L"Microsoft YaHei", 18);
    CHECK(a.isValid());
    CHECK(a.id == b.id);
}

TEST_CASE(ui_font_family_sizes_are_distinct_faces)
{
    if (!msyhAvailable()) {
        return;
    }
    FontHarness h;
    CHECK(h.init());

    const ayt::font::FontHandle s16 = h.ui.getFontHandle(L"Microsoft YaHei", 16);
    const ayt::font::FontHandle s18 = h.ui.getFontHandle(L"Microsoft YaHei", 18);
    CHECK(s16.isValid());
    CHECK(s18.isValid());
    CHECK(s16.id != s18.id);
}

TEST_CASE(ui_font_family_shaping_preserves_utf16_clusters_and_rtl_order)
{
    if (!segoeAvailable()) return;
    FontHarness h;
    CHECK(h.init());

    ayt::ui::IRenderBackend::TextStyle style;
    style.fontFamily = L"Segoe UI";
    const std::wstring supplementary = L"\U0001F600A";
    const auto emoji = h.ui.shapeText(supplementary, 18, style);
    CHECK(!emoji.clusters.empty());
    size_t invalidClusterCount = 0;
    for (const auto& cluster : emoji.clusters) {
        if (cluster.sourceStart >= supplementary.size() || cluster.sourceStart == 1u
            || cluster.sourceStart + cluster.sourceLength > supplementary.size()) {
            ++invalidClusterCount;
        }
    }
    CHECK(invalidClusterCount == 0u);

    style.direction = ayt::ui::TextDirection::RightToLeft;
    style.language = "he";
    const auto rtl = h.ui.shapeText(L"\u05E9\u05DC\u05D5\u05DD", 18, style);
    CHECK(rtl.rightToLeft);
    CHECK(rtl.clusters.size() >= 2u);
    size_t visualOrderViolationCount = 0;
    for (size_t i = 1; i < rtl.clusters.size(); ++i) {
        if (rtl.clusters[i - 1u].sourceStart < rtl.clusters[i].sourceStart) {
            ++visualOrderViolationCount;
        }
    }
    CHECK(visualOrderViolationCount == 0u);
}

TEST_CASE(ui_font_family_letter_spacing_is_once_per_shaping_cluster)
{
    if (!segoeAvailable()) return;
    FontHarness h;
    CHECK(h.init());

    ayt::ui::IRenderBackend::TextStyle style;
    style.fontFamily = L"Segoe UI";
    const auto base = h.ui.shapeText(L"office", 20, style);
    CHECK(!base.clusters.empty());
    style.letterSpacing = 5;
    const auto spaced = h.ui.shapeText(L"office", 20, style);
    CHECK(spaced.clusters.size() == base.clusters.size());
    const float expected = base.metrics.width
        + 5.0f * static_cast<float>(base.clusters.size());
    CHECK(std::abs(spaced.metrics.width - expected) < 0.05f);
}

TEST_CASE(ui_font_family_multiple_faces_use_independent_atlas_batches)
{
    if (!segoeAvailable() || !msyhAvailable()) return;
    FontHarness h;
    CHECK(h.init());

    ayt::ui::IRenderBackend::TextStyle latin;
    latin.fontFamily = L"Segoe UI";
    latin.fontWeight = 700;
    latin.bold = true;
    ayt::ui::IRenderBackend::TextStyle cjk;
    cjk.fontFamily = L"Microsoft YaHei";

    h.ui.beginFrame();
    h.ui.drawText(ayt::math::FRectangle(10, 10, 260, 42), L"Bold Latin", 18, latin);
    h.ui.drawText(ayt::math::FRectangle(10, 52, 260, 84), L"\u591A\u5B57\u4F53", 18, cjk);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 2);
}

} // TEST_SUITE
