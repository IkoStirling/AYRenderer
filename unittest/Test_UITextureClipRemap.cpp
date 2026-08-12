// Test_UITextureClipRemap.cpp — textured quads under a clip must remap
// their UVs by the clip fraction (crop, not stretch). Dispatch asserted
// on the Noop backend; a partial clip must not change the run structure.

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYUIRenderBackend.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct TexHarness {
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

    ~TexHarness()
    {
        ui.shutdown();
        renderer.shutdown();
    }
};

// 16x16 BGRA gradient (left warm → right cool), distinct from the white
// texture so UV remap mistakes would be visible on a real GPU.
std::vector<unsigned char> makeGradientTexture()
{
    std::vector<unsigned char> px;
    px.reserve(16u * 16u * 4u);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            px.push_back(static_cast<unsigned char>((x * 255) / 15));          // B
            px.push_back(static_cast<unsigned char>(128));                     // G
            px.push_back(static_cast<unsigned char>(((15 - x) * 255) / 15));   // R
            px.push_back(static_cast<unsigned char>(((y * 255) / 15) * 4 / 5 + 51));  // A
        }
    }
    return px;
}

} // namespace

TEST_SUITE(UITextureClipRemapTests)

TEST_CASE(ui_texture_clip_remap_draw_rect_one_call)
{
    TexHarness h;
    CHECK(h.init());

    std::vector<unsigned char> px = makeGradientTexture();
    void* tex = h.ui.createUiTexture(16, 16, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    // Clip cuts the right half of the 200-wide texture draw.
    h.ui.pushClip(ayt::math::FRectangle(100, 100, 200, 200));
    h.ui.drawRect(ayt::math::FRectangle(100, 100, 300, 200), tex,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.popClip();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_texture_clip_remap_draw_with_alpha_one_call)
{
    TexHarness h;
    CHECK(h.init());

    std::vector<unsigned char> px = makeGradientTexture();
    void* tex = h.ui.createUiTexture(16, 16, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(100, 100, 200, 200));
    h.ui.drawWithAlpha(ayt::math::FRectangle(100, 100, 300, 200), tex, 0.5f);
    h.ui.popClip();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_texture_clip_remap_nine_patch_one_call)
{
    TexHarness h;
    CHECK(h.init());

    std::vector<unsigned char> px = makeGradientTexture();
    void* tex = h.ui.createUiTexture(16, 16, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(150, 100, 350, 300));
    // 9-patch spans 100..400 x 100..400; the clip cuts its right/left
    // slices — all surviving slices must still land in one run.
    h.ui.drawNinePatch(ayt::math::FRectangle(100, 100, 400, 400), tex,
                       ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                       ayt::math::FVector4(4.0f, 4.0f, 4.0f, 4.0f));
    h.ui.popClip();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_texture_clip_remap_fully_clipped_culled)
{
    TexHarness h;
    CHECK(h.init());

    std::vector<unsigned char> px = makeGradientTexture();
    void* tex = h.ui.createUiTexture(16, 16, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(100, 100, 200, 200));
    h.ui.drawRect(ayt::math::FRectangle(400, 400, 500, 500), tex,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.popClip();
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 0);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_texture_clip_remap_no_clip_unchanged)
{
    TexHarness h;
    CHECK(h.init());

    std::vector<unsigned char> px = makeGradientTexture();
    void* tex = h.ui.createUiTexture(16, 16, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.drawRect(ayt::math::FRectangle(100, 100, 300, 200), tex,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

} // TEST_SUITE
