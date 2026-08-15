// Test_UIFontFamily.cpp — getFontHandle(familyName, baseSize) honors the
// family dimension: known families resolve to their own face (lazily
// registered per size), unknown/empty names fall back to the size-keyed
// default face (legacy behavior).

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYRenderer/UIRenderBackend.h"

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

} // TEST_SUITE
