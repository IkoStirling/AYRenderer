#include <AYRenderer.h>
#include <AYRenderer/UIRenderBackend.h>
#include "detail/PreviewSceneValidation.h"
#include <AYTest.h>
#include <limits>

using namespace ayt::render;
using ayt::ui::IRenderBackend;

TEST_SUITE(AYRenderer_PreviewScene)
TEST_CASE(preview_camera_rejects_nonfinite_inputs_and_display_uses_main_ranges)
{
    PreviewSceneCamera camera;
    CHECK(detail::validPreviewCamera(camera));
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    camera.view.ptr()[3] = nan;
    CHECK_FALSE(detail::validPreviewCamera(camera));
    camera = {};
    camera.projection.ptr()[15] = inf;
    CHECK_FALSE(detail::validPreviewCamera(camera));
    camera = {};
    camera.position.y = nan;
    CHECK_FALSE(detail::validPreviewCamera(camera));
    PreviewSceneSettings settings;
    settings.exposure = nan;
    settings.gamma = inf;
    settings.lightDirection.x = nan;
    settings.lightColor.z = inf;
    settings.toneMap = static_cast<PreviewToneMap>(255);
    const auto clean = detail::sanitizePreviewSettings(settings);
    CHECK(clean.exposure == 1);
    CHECK(clean.gamma == 2.2f);
    CHECK(clean.toneMap == PreviewToneMap::None);
    CHECK(clean.lightDirection.x == PreviewSceneSettings{}.lightDirection.x);
    CHECK(clean.lightColor.z == 1);
    settings.exposure = 500;
    settings.gamma = .01f;
    CHECK(detail::sanitizePreviewSettings(settings).exposure == 64);
    CHECK(detail::sanitizePreviewSettings(settings).gamma == .1f);
}
TEST_CASE(preview_budget_rejection_skips_callback_and_hdr_leases_release_on_resize_and_teardown)
{
    Renderer renderer;
    InitDesc init;
    init.backend = Backend::Noop;
    init.width = 320;
    init.height = 240;
    init.vsync = false;
    init.msaa = 0;
    CHECK(renderer.initialize(init));
    if (!renderer.isInitialized())
        return;
    UIRenderBackend ui;
    CHECK(ui.initialize(renderer));
    ui.setFramebufferSize(320, 240);
    const auto target = ui.createRenderTarget(32, 32, false);
    CHECK(target.isValid());
    CHECK(ui.getLayerCacheStats().liveTargetLeases == 1);
    RenderScene scene;
    PreviewSceneCamera camera;
    PreviewSceneSettings display;
    display.displayTransform = true;
    int callbacks = 0;
    const auto before = [&](uint16_t) { ++callbacks; };
    renderer.beginFrame();
    ui.beginFrame();
    auto invalid = camera;
    invalid.position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(ui.renderScenePreview(target, scene, invalid, 0, display, before));
    CHECK(callbacks == 0);
    CHECK(ui.getLayerCacheStats().liveTargetLeases == 1);
    // Leave room only for the existing RGBA8+depth target, not the HDR pair.
    ui.setLayerCacheBudgetBytes(ui.getLayerCacheStats().allocatedTargetBytes);
    CHECK_FALSE(ui.renderScenePreview(target, scene, camera, 0, display, before));
    CHECK(callbacks == 0);
    CHECK(ui.getLayerCacheStats().liveTargetLeases == 1);
    CHECK(ui.getLayerCacheStats().allocationFailures >= 1);
    ui.setLayerCacheBudgetBytes(1024 * 1024);
    // Noop deliberately skips PostProcess; lease/callback ownership is still testable.
    CHECK_FALSE(ui.renderScenePreview(target, scene, camera, 0, display, before));
    CHECK(callbacks == 1);
    CHECK(ui.getLayerCacheStats().liveTargetLeases == 2);
    CHECK(ui.renderScenePreview(target, scene, camera, 0, before));
    CHECK(callbacks == 2);
    ui.endFrame();
    renderer.endFrame();
    IRenderBackend::RenderTargetDesc resized;
    resized.width = 40;
    resized.height = 24;
    CHECK(ui.resizeRenderTarget(target, resized));
    CHECK(ui.getLayerCacheStats().liveTargetLeases == 1);
    renderer.beginFrame();
    ui.beginFrame();
    CHECK_FALSE(ui.renderScenePreview(target, scene, camera, 0, display, before));
    CHECK(ui.getLayerCacheStats().liveTargetLeases == 2);
    ui.releaseRenderTarget(target);
    CHECK(ui.getLayerCacheStats().liveTargetLeases == 0);
    CHECK_FALSE(ui.renderScenePreview(target, scene, camera, 0, display, before));
    CHECK(callbacks == 3);
    ui.endFrame();
    renderer.endFrame();
    ui.shutdown();
    renderer.shutdown();
}
TEST_SUITE_END
