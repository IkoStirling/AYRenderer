// Test_UISdfShader.cpp — P2 SDF rounded-rect shader: drawBorderRect and
// drawRectShadow each submit a single SDF draw call (the P1 8-rect
// decomposition / offset-flat-rect shadow are gone). If the SDF program
// fails to compile, UiGpuContext::initialize fails and the first CHECK
// here goes red — shader drift cannot pass silently. Noop backend: we can
// only assert dispatch counts, geometry is Gallery-verified.

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

} // namespace

TEST_SUITE(UISdfShaderTests)

TEST_CASE(ui_sdf_border_one_draw_call)
{
    Harness h;
    CHECK(h.init());  // SDF program must compile or init() is false

    h.ui.beginFrame();
    h.ui.drawBorderRect(ayt::math::FRectangle(10.0f, 10.0f, 190.0f, 110.0f),
                        ayt::math::FVector4(1.0f, 0.5f, 0.2f, 1.0f), 4.0f, 8.0f);
    h.ui.endFrame();
    // One SDF item — the old 8-rect decomposition would be 8 calls.
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_border_zero_radius_single_call)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawBorderRect(ayt::math::FRectangle(10.0f, 10.0f, 190.0f, 110.0f),
                        ayt::math::FVector4(0.2f, 0.6f, 1.0f, 1.0f), 2.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_border_degenerate_fill_one_call)
{
    Harness h;
    CHECK(h.init());

    // Border as wide as the rect degenerates to a solid fill (Flat item).
    h.ui.beginFrame();
    h.ui.drawBorderRect(ayt::math::FRectangle(10.0f, 10.0f, 210.0f, 210.0f),
                        ayt::math::FVector4(0.1f, 0.8f, 0.3f, 1.0f), 100.0f, 8.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_shadow_one_draw_call)
{
    Harness h;
    CHECK(h.init());

    ayt::ui::IRenderBackend::ShadowStyle shadow;
    shadow.color       = ayt::math::FVector4(0.0f, 0.0f, 0.0f, 0.5f);
    shadow.offset      = ayt::math::FVector2(3.0f, 3.0f);
    shadow.blurRadius  = 4.0f;
    shadow.cornerRadius = 8.0f;

    h.ui.beginFrame();
    h.ui.drawRectShadow(ayt::math::FRectangle(10.0f, 10.0f, 110.0f, 80.0f), shadow);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_border_and_shadow_two_calls)
{
    Harness h;
    CHECK(h.init());

    ayt::ui::IRenderBackend::ShadowStyle shadow;
    shadow.color      = ayt::math::FVector4(0.0f, 0.0f, 0.0f, 0.4f);
    shadow.blurRadius = 6.0f;

    h.ui.beginFrame();
    h.ui.drawRectShadow(ayt::math::FRectangle(10.0f, 10.0f, 210.0f, 160.0f), shadow);
    h.ui.drawBorderRect(ayt::math::FRectangle(10.0f, 10.0f, 210.0f, 160.0f),
                        ayt::math::FVector4(0.9f, 0.9f, 0.9f, 1.0f), 3.0f, 12.0f);
    h.ui.endFrame();
    // Each SDF item submits alone: shadow + border = 2 (never merged).
    CHECK(h.ui.getDrawCallCount() == 2);
}

TEST_CASE(ui_sdf_rect_border_text_three_calls)
{
    if (!systemFontAvailable()) {
        std::fprintf(stderr, "[UISdfShader text test] SKIP: system UI font not found\n");
        return;
    }
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 60.0f, 40.0f);          // Flat run
    h.ui.drawBorderRect(ayt::math::FRectangle(80.0f, 10.0f, 180.0f, 60.0f),
                        ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f), 2.0f, 6.0f);  // SDF
    h.ui.drawText(ayt::math::FRectangle(10.0f, 80.0f, 200.0f, 106.0f), L"Play", 14,
                  ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));                     // glyph run
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 3);
}

TEST_CASE(ui_sdf_frames_isolated_no_stale)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawBorderRect(ayt::math::FRectangle(10.0f, 10.0f, 190.0f, 110.0f),
                        ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f), 4.0f, 8.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    // Frame 2 draws nothing SDF — no stale items leaking across frames.
    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 60.0f, 40.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_border_inside_clip_single_call)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(0.0f, 0.0f, 100.0f, 60.0f));
    h.ui.drawBorderRect(ayt::math::FRectangle(10.0f, 10.0f, 190.0f, 110.0f),
                        ayt::math::FVector4(0.0f, 1.0f, 1.0f, 1.0f), 3.0f, 8.0f);
    h.ui.popClip();
    h.ui.endFrame();
    // Clip is applied at item-record time; the SDF quad still submits once.
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_zero_border_skipped)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawBorderRect(ayt::math::FRectangle(10.0f, 10.0f, 190.0f, 110.0f),
                        ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, 8.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 0);
}

TEST_CASE(ui_sdf_transparent_shadow_skipped)
{
    Harness h;
    CHECK(h.init());

    ayt::ui::IRenderBackend::ShadowStyle shadow;
    shadow.color      = ayt::math::FVector4(0.0f, 0.0f, 0.0f, 0.0f);  // invisible
    shadow.blurRadius = 4.0f;

    h.ui.beginFrame();
    h.ui.drawRectShadow(ayt::math::FRectangle(10.0f, 10.0f, 110.0f, 80.0f), shadow);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 0);
}

TEST_SUITE_END
