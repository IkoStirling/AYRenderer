// Test_UIBatchBaseline.cpp — draw-call baseline for a Gallery-style UI
// frame: flat background + 5 SDF-skin buttons (rounded fill + 1px border
// + text) + 2 shadow panels + 1 popup menu plate. Records the current
// dispatch cost; the batch knife (SDF same-param run merging) is measured
// against these numbers, so the CHECK is a deliberate pin of today's
// cost, not an aspirational target.

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

} // namespace

TEST_SUITE(UIBatchBaselineTests)

TEST_CASE(ui_batch_baseline_gallery_frame)
{
    UiHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    flat(h.ui, 0.0f, 0.0f, 800.0f, 600.0f);

    // 5 same-style buttons: rounded fill (r=6) + 1px border (r=6) + "OK".
    for (int i = 0; i < 5; ++i) {
        const float y = 20.0f + static_cast<float>(i) * 60.0f;
        buttonFill(h.ui, 40.0f, y, 120.0f, 40.0f);
        buttonBorder(h.ui, 40.0f, y, 120.0f, 40.0f);
        buttonText(h.ui, 60.0f, y + 12.0f, 80.0f, 20.0f);
    }

    // 2 panels: soft shadow + rounded fill (r=8).
    for (int i = 0; i < 2; ++i) {
        const float x = 240.0f + static_cast<float>(i) * 220.0f;
        panel(h.ui, x, 20.0f, 200.0f, 200.0f);
    }

    // Popup menu plate: rounded fill + border + two text rows.
    buttonFill(h.ui, 250.0f, 260.0f, 180.0f, 120.0f);  // r=6 plate fill
    buttonBorder(h.ui, 250.0f, 260.0f, 180.0f, 120.0f);
    buttonText(h.ui, 270.0f, 280.0f, 140.0f, 20.0f);
    buttonText(h.ui, 270.0f, 310.0f, 140.0f, 20.0f);

    h.ui.endFrame();

    const int count = h.ui.getDrawCallCount();

    // Pinned today's cost (measured 2026-08-12, batch knife landed):
    // 23 = flat(1) + 5 buttons × (fill SDF + border SDF + text run) +
    // 2 panels × (shadow SDF + fill SDF) + menu (fill SDF + border SDF +
    // 1 text run — the two adjacent "OK" rows merge into one glyph run).
    // NOTE: a real frame interleaves fill/stroke/text per control, so
    // consecutive-run SDF merging (ui_sdf_batch_* tests) gets no purchase
    // here — the runs are already 1-quad long. The knife's payoff is
    // CONSECUTIVE same-param SDF (palette swatches, status dots, stacked
    // plates), which this frame deliberately does not contain.
    CHECK(count == 23);
}

TEST_SUITE_END
