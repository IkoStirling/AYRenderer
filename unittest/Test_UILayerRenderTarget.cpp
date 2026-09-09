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
    CHECK(UIRenderBackend::kLastLayerViewId == 245);
    CHECK(UIRenderBackend::kMaxOffscreenPaintsPerFrame == 220);
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
    const auto earlyStats = ui.getLayerCacheStats();
    CHECK(earlyStats.layerCreates == 1u);
    CHECK(earlyStats.fullPaints == 1u);
    CHECK(earlyStats.partialPaints == 1u);
    CHECK(earlyStats.composites == 3u);
    CHECK(earlyStats.cacheHits == 1u);
    CHECK(earlyStats.repaintPixelArea == 49284u);
    CHECK(earlyStats.liveLayers == 1u);
    CHECK(earlyStats.liveTargetLeases == 1u);

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
    // 26 through 246. This is a per-frame pass capacity, not a promise that
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

TEST_CASE(UIRenderBackend_StrictLayerBudgetDegradesLruAndKeepsLogicalHandles)
{
    Renderer renderer;
    InitDesc init;
    init.backend = Backend::Noop;
    init.width = 320;
    init.height = 180;
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
    ui.setFramebufferSize(320, 180);
    constexpr size_t kOneLayerBudget = 64u * 64u * 8u;
    ui.setLayerCacheBudgetBytes(kOneLayerBudget);

    IRenderBackend::LayerDesc desc;
    desc.logicalBounds = FRectangle(0.0f, 0.0f, 64.0f, 64.0f);
    desc.dpiScale = 1.0f;
    desc.clearMode = IRenderBackend::LayerClearMode::Transparent;
    const auto first = ui.createLayer(desc);
    const auto second = ui.createLayer(desc);
    CHECK_TRUE(first.isValid());
    CHECK_TRUE(second.isValid());

    IRenderBackend::LayerPaint paint;
    paint.damage = desc.logicalBounds;
    paint.fullRedraw = true;
    renderer.beginFrame();
    ui.beginFrame();
    CHECK_TRUE(ui.beginLayerPaint(first, paint));
    ui.drawRect(desc.logicalBounds, FVector4(1, 0, 0, 1));
    ui.endLayerPaint(first);
    ui.compositeLayer(first, desc.logicalBounds);
    CHECK_FALSE(ui.beginLayerPaint(second, paint));
    ui.endFrame();
    renderer.endFrame();

    auto stats = ui.getLayerCacheStats();
    CHECK(stats.liveLayers == 2u);
    CHECK(stats.allocatedTargetBytes == kOneLayerBudget);
    CHECK(stats.targetBudgetBytes == kOneLayerBudget);
    CHECK(stats.degradedLayers == 1u);
    CHECK(stats.allocationFailures == 1u);
    CHECK_TRUE(ui.isLayerDirty(first));
    CHECK_TRUE(ui.isLayerDirty(second));

    // The evicted lease is quarantined for two renderer frames. Once safe,
    // the pool reclaims it and the second logical LayerHandle paints without
    // being destroyed/recreated by the caller.
    renderer.beginFrame();
    ui.beginFrame();
    ui.endFrame();
    renderer.endFrame();

    renderer.beginFrame();
    ui.beginFrame();
    CHECK_TRUE(ui.beginLayerPaint(second, paint));
    ui.drawRect(desc.logicalBounds, FVector4(0, 1, 0, 1));
    ui.endLayerPaint(second);
    ui.compositeLayer(second, desc.logicalBounds);
    ui.endFrame();
    renderer.endFrame();
    CHECK_FALSE(ui.isLayerDirty(second));
    stats = ui.getLayerCacheStats();
    CHECK(stats.targetAllocations == 1u);
    CHECK(stats.targetReuses >= 1u);
    CHECK(stats.liveTargetLeases == 1u);

    ui.releaseLayer(first);
    ui.releaseLayer(second);
    ui.shutdown();
    renderer.shutdown();
}

TEST_SUITE_END
