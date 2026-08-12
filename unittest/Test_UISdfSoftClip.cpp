// Test_UISdfSoftClip.cpp — SDF soft-clip: every SDF item records the clip
// rect at record time and the shader fades coverage across the clip edge
// (scroll seams no longer hard-cut stroke/shadow AA). Noop backend asserts
// dispatch + that the run-merge batch behavior is untouched by clip.

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYUIRenderBackend.h"

#include <cstdio>

namespace {

struct SdfHarness {
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

    ~SdfHarness()
    {
        ui.shutdown();
        renderer.shutdown();
    }
};

} // namespace

TEST_SUITE(UISdfSoftClipTests)

TEST_CASE(ui_sdf_soft_clip_rounded_rect_one_call)
{
    SdfHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(100, 100, 300, 200));
    h.ui.drawRoundedRect(ayt::math::FRectangle(150, 120, 250, 180), ayt::math::FVector4(0.2f, 0.4f, 0.6f, 1.0f), 8);
    h.ui.popClip();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_soft_clip_border_and_shadow_one_call_each)
{
    SdfHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(100, 100, 300, 200));
    h.ui.drawBorderRect(ayt::math::FRectangle(150, 120, 250, 180), ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f), 2, 6);
    ayt::ui::IRenderBackend::ShadowStyle shadow;
    shadow.color   = ayt::math::FVector4(0.0f, 0.0f, 0.0f, 0.5f);
    shadow.offset  = ayt::math::FVector2(4, 4);
    shadow.blurRadius = 4;
    h.ui.drawRectShadow(ayt::math::FRectangle(200, 140, 280, 180), shadow);
    h.ui.popClip();
    h.ui.endFrame();
    // Border and shadow are different SDF param sets → two runs → 2 calls.
    CHECK(h.ui.getDrawCallCount() == 2);
}

TEST_CASE(ui_sdf_soft_clip_outside_clip_culled)
{
    SdfHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(100, 100, 300, 200));
    h.ui.drawRoundedRect(ayt::math::FRectangle(400, 400, 500, 500), ayt::math::FVector4(0.2f, 0.4f, 0.6f, 1.0f), 8);
    h.ui.popClip();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 0);
}

TEST_CASE(ui_sdf_soft_clip_does_not_break_run_merging)
{
    SdfHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(100, 100, 700, 500));
    // Same-param borders under one clip still merge into a single run —
    // the clip rides per-vertex, not in the run key.
    for (int i = 0; i < 3; ++i) {
        h.ui.drawBorderRect(ayt::math::FRectangle(150.0f + i * 60.0f, 120, 210.0f + i * 60.0f, 180),
                            ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f), 2, 6);
    }
    h.ui.popClip();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_sdf_soft_clip_no_clip_still_one_call)
{
    SdfHarness h;
    CHECK(h.init());

    h.ui.beginFrame();
    h.ui.drawBorderRect(ayt::math::FRectangle(150, 120, 250, 180), ayt::math::FVector4(1.0f, 1.0f, 1.0f, 1.0f), 2, 6);
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

} // TEST_SUITE
