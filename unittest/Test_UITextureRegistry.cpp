// Test_UITextureRegistry.cpp — P3 texture registry + 9-patch: createUiTexture
// uploads a BGRA8 texture (linear + clamp wrap), drawRect / drawWithAlpha /
// drawNinePatch consume the opaque handle, releaseUiTexture frees it at
// refcount 0. The registry is persistent across frames (beginFrame does not
// touch it) and fully released at shutdown. Noop backend: dispatch counts
// only — the 9-slice geometry itself is Gallery-verified.

#include "AYRenderer.h"
#include "AYTest.h"
#include "AYUIRenderBackend.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

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

std::vector<uint8_t> makeBgra(uint16_t w, uint16_t h)
{
    return std::vector<uint8_t>(static_cast<size_t>(w) * h * 4u, 255u);
}

} // namespace

TEST_SUITE(UITextureRegistryTests)

TEST_CASE(ui_texture_lifecycle_single_run)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(64, 64);
    void* tex = h.ui.createUiTexture(64, 64, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.drawRect(ayt::math::FRectangle(10.0f, 10.0f, 90.0f, 90.0f), tex,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.drawWithAlpha(ayt::math::FRectangle(100.0f, 10.0f, 200.0f, 90.0f), tex, 0.5f);
    h.ui.endFrame();
    // Same texture + blend state → one run, one draw call.
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_texture_persists_across_frames)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(32, 32);
    void* tex = h.ui.createUiTexture(32, 32, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.drawRect(ayt::math::FRectangle(0.0f, 0.0f, 32.0f, 32.0f), tex,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    // Frame 2: beginFrame must NOT clear the registry — handle stays valid.
    h.ui.beginFrame();
    h.ui.drawRect(ayt::math::FRectangle(0.0f, 0.0f, 32.0f, 32.0f), tex,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_texture_double_release_noop_and_fallback)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(32, 32);
    void* tex = h.ui.createUiTexture(32, 32, px.data());
    CHECK(tex != nullptr);

    h.ui.releaseUiTexture(tex);
    h.ui.releaseUiTexture(tex);  // double release — safe no-op

    // Released handle draws the gray fallback (still one call, no crash).
    h.ui.beginFrame();
    h.ui.drawRect(ayt::math::FRectangle(0.0f, 0.0f, 32.0f, 32.0f), tex,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_texture_unknown_handle_gray_fallback)
{
    Harness h;
    CHECK(h.init());

    h.ui.beginFrame();
    void* fake = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
    h.ui.drawRect(ayt::math::FRectangle(0.0f, 0.0f, 64.0f, 64.0f), fake,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.drawWithAlpha(ayt::math::FRectangle(0.0f, 0.0f, 64.0f, 64.0f), nullptr, 0.25f);
    h.ui.drawNinePatch(ayt::math::FRectangle(0.0f, 0.0f, 64.0f, 64.0f), fake,
                       ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                       ayt::math::FVector4(8.0f, 8.0f, 8.0f, 8.0f));
    h.ui.endFrame();
    // All three fall back to gray Flat rects — same texture/state → one run.
    CHECK(h.ui.getDrawCallCount() == 1);
}

TEST_CASE(ui_texture_create_invalid_args)
{
    Harness h;
    CHECK(h.init());

    CHECK(h.ui.createUiTexture(0, 16, makeBgra(16, 16).data()) == nullptr);
    CHECK(h.ui.createUiTexture(16, 16, nullptr) == nullptr);
}

TEST_CASE(ui_nine_patch_single_run)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(64, 64);
    void* tex = h.ui.createUiTexture(64, 64, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    // padding in texture pixels: 16px corners on the 64px texture.
    h.ui.drawNinePatch(ayt::math::FRectangle(20.0f, 20.0f, 420.0f, 220.0f), tex,
                       ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                       ayt::math::FVector4(16.0f, 16.0f, 16.0f, 16.0f));
    h.ui.endFrame();
    // 9 quads, one texture + one blend state → one run.
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_nine_patch_zero_padding_single_quad)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(64, 64);
    void* tex = h.ui.createUiTexture(64, 64, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.drawNinePatch(ayt::math::FRectangle(20.0f, 20.0f, 120.0f, 80.0f), tex,
                       ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                       ayt::math::FVector4(0.0f, 0.0f, 0.0f, 0.0f));
    h.ui.endFrame();
    // Degenerate padding → single stretched quad (still one call).
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_nine_patch_padding_larger_than_bounds)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(64, 64);
    void* tex = h.ui.createUiTexture(64, 64, px.data());
    CHECK(tex != nullptr);

    // 40px corners on a 60x40 rect: padding scaled down to fit, no
    // degenerate geometry, still one run.
    h.ui.beginFrame();
    h.ui.drawNinePatch(ayt::math::FRectangle(10.0f, 10.0f, 70.0f, 50.0f), tex,
                       ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                       ayt::math::FVector4(40.0f, 40.0f, 40.0f, 40.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_nine_patch_run_boundaries)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(32, 32);
    void* tex = h.ui.createUiTexture(32, 32, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.drawNinePatch(ayt::math::FRectangle(10.0f, 10.0f, 110.0f, 60.0f), tex,
                       ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                       ayt::math::FVector4(8.0f, 8.0f, 8.0f, 8.0f));       // texture run
    h.ui.drawRect(ayt::math::FRectangle(120.0f, 10.0f, 180.0f, 60.0f),
                  ayt::math::FVector4(0.3f, 0.3f, 0.3f, 1.0f));            // white run
    h.ui.drawNinePatch(ayt::math::FRectangle(190.0f, 10.0f, 290.0f, 60.0f), tex,
                       ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                       ayt::math::FVector4(8.0f, 8.0f, 8.0f, 8.0f));       // texture run again
    h.ui.endFrame();
    // Texture↔white switches break the run: 3 calls (never merged).
    CHECK(h.ui.getDrawCallCount() == 3);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_nine_patch_clip_single_run)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(64, 64);
    void* tex = h.ui.createUiTexture(64, 64, px.data());
    CHECK(tex != nullptr);

    h.ui.beginFrame();
    h.ui.pushClip(ayt::math::FRectangle(0.0f, 0.0f, 200.0f, 100.0f));
    h.ui.drawNinePatch(ayt::math::FRectangle(50.0f, 50.0f, 450.0f, 250.0f), tex,
                       ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f),
                       ayt::math::FVector4(16.0f, 16.0f, 16.0f, 16.0f));
    h.ui.popClip();
    h.ui.endFrame();
    // Bounds are clipped once at record time; all 9 slices stay inside.
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex);
}

TEST_CASE(ui_texture_registry_shutdown_reinit)
{
    Harness h;
    CHECK(h.init());

    std::vector<uint8_t> px = makeBgra(32, 32);
    void* tex = h.ui.createUiTexture(32, 32, px.data());
    CHECK(tex != nullptr);

    // Backend shutdown releases the registry (texture freed with it).
    h.ui.shutdown();
    CHECK(!h.ui.isInitialized());
    h.ui.releaseUiTexture(tex);  // stale handle — safe no-op

    // Re-init on the same renderer: registry is fresh, handles were not
    // reused (nextHandle keeps counting, so stale handles stay dead).
    CHECK(h.ui.initialize(h.renderer));
    h.ui.setFramebufferSize(800, 600);

    void* tex2 = h.ui.createUiTexture(16, 16, px.data());
    CHECK(tex2 != nullptr);
    CHECK(tex2 != tex);

    h.ui.beginFrame();
    h.ui.drawRect(ayt::math::FRectangle(0.0f, 0.0f, 32.0f, 32.0f), tex2,
                  ayt::math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
    h.ui.endFrame();
    CHECK(h.ui.getDrawCallCount() == 1);

    h.ui.releaseUiTexture(tex2);
}

TEST_SUITE_END
