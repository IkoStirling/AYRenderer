// Test_UIUnifiedBatch.cpp — P0 unified UiItem batch: one ordered draw
// list, consecutive-run grouping at flush, no mid-frame z-order flushes.

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

struct UiHarness {
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

    ~UiHarness()
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

TEST_SUITE(UIUnifiedBatchTests)

TEST_CASE(ui_unified_batch_rects_one_draw_call)
{
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    for (int i = 0; i < 10; ++i) {
        drawRectAt(h.ui, static_cast<float>(i) * 12.0f, 10.0f, 8.0f, 8.0f);
    }
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_unified_batch_text_and_rects_two_runs)
{
    if (!systemFontAvailable()) {
        std::fprintf(stderr, "[UIUnifiedBatch text test] SKIP: system UI font not found\n");
        return;
    }
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);
    h.ui.drawText(ayt::math::FRectangle(60.0f, 10.0f, 200.0f, 36.0f), L"Play", 14,
                  ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    // rect run (white) + glyph run (atlas) = 2 draw calls.
    CHECK(h.ui.getDrawCallCount() == 2);
}

TEST_CASE(ui_unified_batch_interleaved_runs_preserve_order)
{
    if (!systemFontAvailable()) {
        std::fprintf(stderr, "[UIUnifiedBatch interleave test] SKIP: system UI font not found\n");
        return;
    }
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);
    h.ui.drawText(ayt::math::FRectangle(60.0f, 10.0f, 200.0f, 36.0f), L"Play", 14,
                  ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
    drawRectAt(h.ui, 210.0f, 10.0f, 40.0f, 40.0f);
    h.ui.endFrame();
    // rect / text / rect: three consecutive runs; never merged (z-order wins).
    CHECK(h.ui.getDrawCallCount() == 3);
}

TEST_CASE(ui_unified_batch_empty_frame_zero)
{
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 0);
}

TEST_CASE(ui_unified_batch_two_frames_no_stale)
{
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);
    drawRectAt(h.ui, 60.0f, 10.0f, 40.0f, 40.0f);
    drawRectAt(h.ui, 110.0f, 10.0f, 40.0f, 40.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.beginFrame();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 0);
}

TEST_CASE(ui_unified_batch_flush_midframe)
{
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);
    h.ui.flushBatches();
    drawRectAt(h.ui, 60.0f, 10.0f, 40.0f, 40.0f);
    h.ui.endFrame();
    // Mid-frame flush submits the first run; the second appends a fresh one.
    CHECK(h.ui.getDrawCallCount() == 2);
}

TEST_CASE(ui_unified_batch_clip_no_extra_draw_calls)
{
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(0.0f, 0.0f, 400.0f, 300.0f));
    drawRectAt(h.ui, 10.0f, 10.0f, 40.0f, 40.0f);
    drawRectAt(h.ui, 60.0f, 10.0f, 40.0f, 40.0f);
    h.ui.popClip();
    h.ui.endFrame();
    // Clip push/pop must not force extra submissions.
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_unified_batch_clip_culls)
{
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(0.0f, 0.0f, 100.0f, 100.0f));
    drawRectAt(h.ui, 200.0f, 200.0f, 40.0f, 40.0f);  // fully outside clip
    h.ui.popClip();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 0);
}

TEST_SUITE_END
