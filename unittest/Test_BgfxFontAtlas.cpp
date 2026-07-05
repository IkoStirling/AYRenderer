// Test_BgfxFontAtlas.cpp — F0 UIRenderBackend + AYFont path (Noop GPU)

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

TEST_SUITE_END
