// Test_UIBatchBaseline.cpp — draw-call baseline for a Gallery-style UI
// frame: flat background + 5 SDF-skin buttons (rounded fill + 1px border
// + text) + 2 shadow panels + 1 popup menu plate. Pins both the optimized
// overlap-aware cost and the original ordered-run fallback cost.

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYRenderer/UIRenderBackend.h"

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

void flat(ayt::render::UIRenderBackend& ui, float x, float y, float w, float h)
{
    ui.drawRect(ayt::math::FRectangle(x, y, x + w, y + h),
                ayt::math::FVector4(0.2f, 0.2f, 0.25f, 1.0f));
}

void buttonFill(ayt::render::UIRenderBackend& ui, float x, float y, float w, float h)
{
    ui.drawRoundedRect(ayt::math::FRectangle(x, y, x + w, y + h),
                       ayt::math::FVector4(0.25f, 0.5f, 0.9f, 1.0f), 6.0f);
}

void buttonBorder(ayt::render::UIRenderBackend& ui, float x, float y, float w, float h)
{
    ui.drawBorderRect(ayt::math::FRectangle(x, y, x + w, y + h),
                      ayt::math::FVector4(0.9f, 0.95f, 1.0f, 1.0f), 1.0f, 6.0f);
}

void buttonText(ayt::render::UIRenderBackend& ui, float x, float y, float w, float h)
{
    ui.drawText(ayt::math::FRectangle(x, y, x + w, y + h), L"OK", 12,
                ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f));
}

void panel(ayt::render::UIRenderBackend& ui, float x, float y, float w, float h)
{
    const ayt::math::FRectangle b(x, y, x + w, y + h);
    ui.drawRectShadow(b, ayt::ui::IRenderBackend::ShadowStyle{
        ayt::math::FVector4(0.0f, 0.0f, 0.0f, 0.4f), ayt::math::FVector2(0.0f, 2.0f),
        4.0f, 8.0f});
    ui.drawRoundedRect(b, ayt::math::FVector4(0.15f, 0.15f, 0.2f, 1.0f), 8.0f);
}

void recordGalleryFrame(ayt::render::UIRenderBackend& ui)
{
    ui.beginFrame();
    flat(ui, 0.0f, 0.0f, 800.0f, 600.0f);

    for (int i = 0; i < 5; ++i) {
        const float y = 20.0f + static_cast<float>(i) * 60.0f;
        buttonFill(ui, 40.0f, y, 120.0f, 40.0f);
        buttonBorder(ui, 40.0f, y, 120.0f, 40.0f);
        buttonText(ui, 60.0f, y + 12.0f, 80.0f, 20.0f);
    }

    for (int i = 0; i < 2; ++i) {
        const float x = 240.0f + static_cast<float>(i) * 220.0f;
        panel(ui, x, 20.0f, 200.0f, 200.0f);
    }

    buttonFill(ui, 250.0f, 260.0f, 180.0f, 120.0f);
    buttonBorder(ui, 250.0f, 260.0f, 180.0f, 120.0f);
    buttonText(ui, 270.0f, 280.0f, 140.0f, 20.0f);
    buttonText(ui, 270.0f, 310.0f, 140.0f, 20.0f);
    ui.endFrame();
}

} // namespace

TEST_SUITE(UIBatchBaselineTests)

TEST_CASE(ui_batch_baseline_gallery_frame)
{
    UiHarness h;
    CHECK(h.init());

    CHECK(h.ui.getBatchMode() == ayt::render::UIRenderBackend::BatchMode::OverlapAware);
    recordGalleryFrame(h.ui);

    // 6 = full-screen flat + r6 fills + r6 borders + atlas text + panel
    // shadows + r8 panel fills. Disjoint controls cross material barriers;
    // the full-screen background remains first because it overlaps all.
    CHECK(h.ui.getDrawCallCount() == 6);
}

TEST_CASE(ui_batch_baseline_ordered_runs_fallback)
{
    UiHarness h;
    CHECK(h.init());
    h.ui.setBatchMode(ayt::render::UIRenderBackend::BatchMode::OrderedRuns);
    recordGalleryFrame(h.ui);

    // Original implementation retained verbatim at the behavioral level.
    CHECK(h.ui.getDrawCallCount() == 23);
}

TEST_SUITE_END
