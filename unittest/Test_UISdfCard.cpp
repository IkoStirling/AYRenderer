// Test_UISdfCard.cpp — the merged card API (shadow+fill+stroke in ONE SDF
// submission), per-corner radii, and BorderStyle::Position → strokeInset
// routing. Dispatch asserted on the Noop backend: shaders don't execute,
// but run structure (draw call counts) proves the layering decisions.

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYUIRenderBackend.h"

#include <cstdio>

namespace {

struct CardHarness {
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

    ~CardHarness()
    {
        ui.shutdown();
        renderer.shutdown();
    }
};

ayt::ui::IRenderBackend::CardStyle makeCard()
{
    ayt::ui::IRenderBackend::CardStyle card;
    card.fillColor        = ayt::math::FVector4(0.9f, 0.5f, 0.2f, 1.0f);
    card.cornerRadius     = ayt::ui::IRenderBackend::CornerRadii(8.0f);
    card.borderColor      = ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    card.borderWidth      = 2.0f;
    card.shadowColor      = ayt::math::FVector4(0.0f, 0.0f, 0.0f, 0.5f);
    card.shadowOffset     = ayt::math::FVector2(4.0f, 4.0f);
    card.shadowBlurRadius = 4.0f;
    return card;
}

} // namespace

TEST_SUITE(UISdfCardTests)

TEST_CASE(ui_sdf_card_all_layers_one_call)
{
    CardHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawCard(ayt::math::FRectangle(100, 100, 300, 200), makeCard());
    h.ui.endFrame();
    // shadow + fill + stroke composite inside the shader — one submission,
    // not the three the layered API needed.
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_card_fill_only_one_call)
{
    CardHarness h;
    CHECK(h.init());

    ayt::ui::IRenderBackend::CardStyle card = makeCard();
    card.borderColor.w = 0.0f;  // stroke off
    card.shadowColor.w = 0.0f;  // shadow off

    h.ui.beginFrame();
    h.ui.drawCard(ayt::math::FRectangle(100, 100, 300, 200), card);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_card_border_only_one_call)
{
    CardHarness h;
    CHECK(h.init());

    ayt::ui::IRenderBackend::CardStyle card = makeCard();
    card.fillColor.w = 0.0f;  // fill off

    h.ui.beginFrame();
    h.ui.drawCard(ayt::math::FRectangle(100, 100, 300, 200), card);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_card_no_layers_zero_calls)
{
    CardHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawCard(ayt::math::FRectangle(100, 100, 300, 200), ayt::ui::IRenderBackend::CardStyle{});
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 0);
}

TEST_CASE(ui_sdf_card_same_params_merge_one_call)
{
    CardHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    const ayt::ui::IRenderBackend::CardStyle card = makeCard();
    h.ui.drawCard(ayt::math::FRectangle(100, 100, 300, 200), card);
    h.ui.drawCard(ayt::math::FRectangle(350, 100, 550, 200), card);
    h.ui.endFrame();
    // Identical SDF params (positions ride per-vertex) → one merged run.
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_card_radius_diff_no_merge)
{
    CardHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    ayt::ui::IRenderBackend::CardStyle a = makeCard();
    ayt::ui::IRenderBackend::CardStyle b = makeCard();
    b.cornerRadius = ayt::ui::IRenderBackend::CornerRadii(16.0f);
    h.ui.drawCard(ayt::math::FRectangle(100, 100, 300, 200), a);
    h.ui.drawCard(ayt::math::FRectangle(350, 100, 550, 200), b);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 2);
}

TEST_CASE(ui_sdf_card_per_corner_fill_dispatch)
{
    CardHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawRoundedRect(ayt::math::FRectangle(100, 100, 300, 200),
                         ayt::math::FVector4(0.2f, 0.4f, 0.6f, 1.0f),
                         ayt::ui::IRenderBackend::CornerRadii(8.0f, 4.0f, 2.0f, 1.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_card_per_corner_border_dispatch)
{
    CardHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawBorderRect(ayt::math::FRectangle(100, 100, 300, 200),
                        ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f), 2.0f,
                        ayt::ui::IRenderBackend::CornerRadii(1.0f, 2.0f, 3.0f, 4.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_card_border_style_position_routes_to_card)
{
    CardHarness h;
    CHECK(h.init());

    // drawRect(BorderStyle) routes through drawCard on this backend — the
    // Position field must reach the SDF stroke (single submission).
    ayt::ui::IRenderBackend::BorderStyle border;
    border.color   = ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f);
    border.width   = 2.0f;
    border.cornerRadius = 6.0f;
    border.position = ayt::ui::IRenderBackend::BorderStyle::Position::Outside;

    h.ui.beginFrame();
    h.ui.drawRect(ayt::math::FRectangle(100, 100, 300, 200), border);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_card_inside_vs_outside_no_merge)
{
    CardHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    ayt::ui::IRenderBackend::CardStyle a = makeCard();
    ayt::ui::IRenderBackend::CardStyle b = makeCard();
    b.borderPosition = ayt::ui::IRenderBackend::BorderStyle::Position::Inside;
    h.ui.drawCard(ayt::math::FRectangle(100, 100, 300, 200), a);
    h.ui.drawCard(ayt::math::FRectangle(350, 100, 550, 200), b);
    h.ui.endFrame();
    // Position maps to strokeInset — a real SDF param — so the runs must
    // NOT merge (else the inside ring would sample the outside shader).
    CHECK(h.ui.getDrawCallCount() == 2);
}

TEST_CASE(ui_sdf_card_scalar_overloads_unchanged)
{
    CardHarness h;
    CHECK(h.init());

    // The scalar entry points must keep their dispatch (regression guard
    // for the wrapper refactor).
    h.ui.beginFrame();
    h.ui.drawRoundedRect(ayt::math::FRectangle(100, 100, 300, 200),
                         ayt::math::FVector4(0.2f, 0.4f, 0.6f, 1.0f), 8.0f);
    h.ui.drawBorderRect(ayt::math::FRectangle(150, 120, 250, 180),
                        ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f), 2.0f, 6.0f);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 2);
}

} // TEST_SUITE
