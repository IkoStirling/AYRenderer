// Test_BgfxFontAtlas.cpp — UIRenderBackend + AYFont (measure + shaped draw)

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

} // namespace

TEST_SUITE(BgfxFontAtlasTests)

TEST_CASE(ui_render_backend_draw_text_with_font)
{
    if (!systemFontAvailable()) {
        std::fprintf(stderr, "[UIRenderBackend font test] SKIP: system UI font not found\n");
        return;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::UIRenderBackend ui;
    CHECK(ui.initialize(renderer));
    ui.setFramebufferSize(800, 600);
    ui.beginFrame();
    ui.drawText(ayt::math::FRectangle(10.0f, 10.0f, 120.0f, 36.0f), L"Play", 14,
                ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    ui.endFrame();
    CHECK(ui.getDrawCallCount() > 0);

    ui.shutdown();
    renderer.shutdown();
}

TEST_CASE(ui_render_backend_measure_text_nonzero)
{
    if (!systemFontAvailable()) {
        std::fprintf(stderr, "[UIRenderBackend measure test] SKIP: system UI font not found\n");
        return;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::UIRenderBackend ui;
    CHECK(ui.initialize(renderer));

    const auto m = ui.measureText(L"Hello", 14);
    CHECK(m.width > 1.0f);
    CHECK(m.height > 1.0f);
    CHECK(m.ascent > 0.0f);

    const auto wide = ui.measureText(L"Hello World", 14);
    CHECK(wide.width > m.width);

    const ayt::font::FontHandle handle = ui.getFontHandle(L"UI", 14);
    CHECK(handle.isValid());
    const ayt::font::FontMetrics fm = ui.getFontMetrics(handle);
    CHECK(fm.lineHeight > 0.0f);

    ui.shutdown();
    renderer.shutdown();
}

TEST_CASE(ui_render_backend_shaped_cjk_draw)
{
    if (!systemFontAvailable()) {
        std::fprintf(stderr, "[UIRenderBackend CJK test] SKIP: system UI font not found\n");
        return;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::UIRenderBackend ui;
    CHECK(ui.initialize(renderer));
    ui.setFramebufferSize(800, 600);

    // "你好" — exercises HarfBuzz path + glyph-index atlas upload.
    const std::wstring cjk = L"\u4f60\u597d";
    const auto metrics = ui.measureText(cjk, 16);
    CHECK(metrics.width > 1.0f);

    ui.beginFrame();
    ui.drawText(ayt::math::FRectangle(10.0f, 40.0f, 200.0f, 40.0f), cjk, 16,
                ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    ui.endFrame();
    CHECK(ui.getDrawCallCount() > 0);

    ui.shutdown();
    renderer.shutdown();
}

TEST_SUITE_END
