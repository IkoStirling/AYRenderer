#include "AYRenderer.h"
#include "AYRenderer/UIRenderBackend.h"
#include "AYTest.h"

using ayt::math::FRectangle;
using ayt::math::FVector2;
using ayt::math::FVector4;
using ayt::render::Backend;
using ayt::render::InitDesc;
using ayt::render::Renderer;
using ayt::render::UIRenderBackend;
using ayt::ui::IRenderBackend;

TEST_SUITE(AYRenderer_UILayerRenderTarget)

TEST_CASE(UIRenderBackend_LayerHandlesComplexPaintReuseAndDeviceReset)
{
    Renderer renderer;
    InitDesc init;
    init.backend = Backend::Noop;
    init.width = 640;
    init.height = 360;
    init.vsync = false;
    init.msaa = 0;
    CHECK_TRUE(renderer.initialize(init));
    if (!renderer.isInitialized()) return;

    UIRenderBackend ui;
    CHECK_TRUE(ui.initialize(renderer));
    if (!ui.isInitialized()) {
        renderer.shutdown();
        return;
    }
    ui.setFramebufferSize(640, 360);
    ui.setUiScale(1.5f);
    CHECK_TRUE(ui.supportsRenderTargets());
    CHECK(UIRenderBackend::kFirstLayerViewId == 26);
    CHECK(UIRenderBackend::kLastLayerViewId == 249);
    CHECK(UIRenderBackend::kMaxOffscreenPaintsPerFrame == 224);
    CHECK(UIRenderBackend::kFirstLayerViewId < UIRenderBackend::kLastLayerViewId);
    CHECK(UIRenderBackend::kLastLayerViewId < UIRenderBackend::kViewId);

    IRenderBackend::LayerDesc desc;
    desc.logicalBounds = FRectangle(20.0f, 30.0f, 220.0f, 130.0f);
    desc.dpiScale = 1.5f;
    desc.hasAlpha = true;
    desc.clearMode = IRenderBackend::LayerClearMode::Transparent;
    const auto layer = ui.createLayer(desc);
    CHECK_TRUE(layer.isValid());
    CHECK_TRUE(ui.isLayerDirty(layer));

    const uint32_t whitePixel = 0xffffffffu;
    void* texture = ui.createUiTexture(1, 1, &whitePixel);
    CHECK(texture != nullptr);

    ui.beginFrame();
    IRenderBackend::LayerPaint paint;
    paint.damage = desc.logicalBounds;
    paint.fullRedraw = true;
    CHECK_TRUE(ui.beginLayerPaint(layer, paint));
    ui.drawRect(desc.logicalBounds, FVector4(0.08f, 0.10f, 0.14f, 1.0f));
    ui.pushClip(FRectangle(24.0f, 34.0f, 216.0f, 126.0f));
    ui.drawGradientRect(FRectangle(28.0f, 38.0f, 100.0f, 78.0f),
                        FVector4(1, 0, 0, 1), FVector4(0, 1, 0, 1),
                        FVector4(0, 0, 1, 1), FVector4(1, 1, 1, 1));
    ui.drawRoundedRect(FRectangle(108.0f, 38.0f, 200.0f, 78.0f),
                       FVector4(0.2f, 0.5f, 0.9f, 1.0f),
                       IRenderBackend::CornerRadii(3, 7, 11, 5));
    ui.drawNinePatch(FRectangle(28.0f, 84.0f, 88.0f, 116.0f), texture,
                     FRectangle(0, 0, 1, 1), FVector4(1, 1, 1, 1));

    const auto path = ui.createPath();
    ui.addPathRoundedRect(path, FRectangle(96.0f, 84.0f, 204.0f, 118.0f), 8.0f);
    ui.setPathFillColor(path, FVector4(0.9f, 0.6f, 0.1f, 1.0f));
    ui.setPathStrokeColor(path, FVector4(1, 1, 1, 1));
    ui.setPathStrokeWidth(path, 2.0f);
    ui.drawPath(path, IRenderBackend::PathFillMode::FillAndStroke);
    ui.pushPathClip(path);
    ui.drawRect(FRectangle(100.0f, 88.0f, 200.0f, 114.0f),
                FVector4(0.3f, 0.1f, 0.6f, 0.6f));
    ui.popClip();
    ui.releasePath(path);
    ui.popClip();
    ui.endLayerPaint(layer);
    CHECK_FALSE(ui.isLayerDirty(layer));
    ui.compositeLayer(layer, desc.logicalBounds, 0.85f);
    ui.endFrame();
    CHECK(ui.getDrawCallCount() > 0);

    // A clean retained layer requires only its composite submission.
    ui.beginFrame();
    CHECK_FALSE(ui.isLayerDirty(layer));
    ui.compositeLayer(layer, desc.logicalBounds);
    ui.endFrame();
    CHECK(ui.getDrawCallCount() == 1);

    // Transparent partial repaint emits an overwrite clear for damage only,
    // then the caller's clipped paint and the final layer composite.
    const FRectangle partialDamage(40.0f, 48.0f, 96.0f, 82.0f);
    ui.invalidateLayer(layer, partialDamage);
    ui.beginFrame();
    IRenderBackend::LayerPaint partialPaint;
    partialPaint.damage = partialDamage;
    partialPaint.fullRedraw = false;
    CHECK_TRUE(ui.beginLayerPaint(layer, partialPaint));
    ui.pushClip(partialDamage);
    ui.drawRect(desc.logicalBounds, FVector4(0.4f, 0.2f, 0.7f, 0.8f));
    ui.popClip();
    ui.endLayerPaint(layer);
    ui.compositeLayer(layer, desc.logicalBounds);
    ui.endFrame();
    CHECK(ui.getDrawCallCount() == 3);
    CHECK_FALSE(ui.isLayerDirty(layer));

    // Renderer resize resets the shared target pool. The public layer
    // handle remains stable but must become dirty before it can composite.
    renderer.resize(800, 450);
    ui.setFramebufferSize(800, 450);
    ui.beginFrame();
    CHECK_TRUE(ui.isLayerDirty(layer));
    CHECK_TRUE(ui.beginLayerPaint(layer, paint));
    ui.drawRect(desc.logicalBounds, FVector4(0.12f, 0.14f, 0.18f, 1.0f));
    ui.endLayerPaint(layer);
    ui.compositeLayer(layer, desc.logicalBounds);
    ui.endFrame();
    CHECK_FALSE(ui.isLayerDirty(layer));

    // Runtime MSAA changes also call bgfx::reset. The shared pool must
    // invalidate the retained target before that reset and lazily reacquire
    // storage while preserving the public layer handle.
    renderer.setMsaaSampleCount(4);
    ui.beginFrame();
    CHECK_TRUE(ui.isLayerDirty(layer));
    CHECK_TRUE(ui.beginLayerPaint(layer, paint));
    ui.drawRect(desc.logicalBounds, FVector4(0.18f, 0.20f, 0.24f, 1.0f));
    ui.endLayerPaint(layer);
    ui.compositeLayer(layer, desc.logicalBounds);
    ui.endFrame();
    CHECK_FALSE(ui.isLayerDirty(layer));

    // The production range uses every currently unreserved bgfx view from
    // 26 through 249. This is a per-frame pass capacity, not a promise that
    // the shared RenderTargetPool can reserve the same number of framebuffer
    // handles exclusively for UI. Repaint one valid target repeatedly to
    // isolate view allocation from the renderer-wide framebuffer budget.
    ui.beginFrame();
    int successfulPaints = 0;
    for (uint16_t i = 0; i < UIRenderBackend::kMaxOffscreenPaintsPerFrame + 1u; ++i) {
        if (ui.beginLayerPaint(layer, paint)) {
            ++successfulPaints;
            ui.endLayerPaint(layer);
        }
    }
    ui.endFrame();
    CHECK(successfulPaints == UIRenderBackend::kMaxOffscreenPaintsPerFrame);

    ui.beginFrame();
    CHECK_TRUE(ui.beginLayerPaint(layer, paint));
    ui.endLayerPaint(layer);
    ui.endFrame();

    ui.releaseUiTexture(texture);
    ui.releaseLayer(layer);
    CHECK_TRUE(ui.isLayerDirty(layer));
    ui.shutdown();
    renderer.shutdown();
}

TEST_SUITE_END
