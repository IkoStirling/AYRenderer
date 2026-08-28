#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/UIRenderBackend.h"

using namespace ayt::math;
using namespace ayt::render;
using namespace ayt::ui;

namespace {

struct VectorPathHarness {
    Renderer renderer;
    UIRenderBackend ui;

    bool init() {
        InitDesc desc;
        desc.backend = Backend::Noop;
        desc.width = 800;
        desc.height = 600;
        if (!renderer.initialize(desc)) return false;
        if (!ui.initialize(renderer)) {
            renderer.shutdown();
            return false;
        }
        ui.setFramebufferSize(800, 600);
        return true;
    }

    ~VectorPathHarness() {
        ui.shutdown();
        renderer.shutdown();
    }
};

void drawSegmentedBarrierFrame(UIRenderBackend& ui) {
    ui.beginFrame();

    auto ordinarySegment = [&ui](float x) {
        ui.drawRect(FRectangle(x, 10, x + 30, 40), FVector4(1, 1, 1, 1));
        ui.drawRoundedRect(FRectangle(x + 50, 10, x + 80, 40),
                           FVector4(0.4f, 0.7f, 1.0f, 1.0f), 5.0f);
        ui.drawRect(FRectangle(x + 100, 10, x + 130, 40), FVector4(1, 1, 1, 1));
    };

    ordinarySegment(10.0f);
    const auto path = ui.createPath();
    ui.addPathEllipse(path, FVector2(300, 30), 18.0f, 18.0f);
    ui.setPathFillColor(path, FVector4(1, 0.4f, 0.2f, 1));
    ui.drawPath(path, PathFillMode::Fill);
    ui.releasePath(path);
    ordinarySegment(420.0f);
    ui.endFrame();
}

} // namespace

TEST_SUITE(AYRenderer_UIVectorPath)

TEST_CASE(vector_path_concave_fill_and_closed_stroke_tessellate) {
    UIRenderBackend backend;
    const auto path = backend.createPath();
    const FVector2 concave[] = {
        {0.0f, 0.0f}, {100.0f, 0.0f}, {45.0f, 40.0f},
        {100.0f, 100.0f}, {0.0f, 100.0f}
    };
    backend.addPathPolygon(path, concave, 5);
    backend.setPathStrokeWidth(path, 4.0f);

    UIRenderBackend::PathDebugInfo info;
    CHECK(backend.getPathDebugInfo(path, info));
    CHECK(info.contourCount == 1u);
    CHECK(info.openContourCount == 0u);
    CHECK(info.fillTriangleCount == 3u);
    CHECK(info.strokeTriangleCount == 10u);
    CHECK_FLOAT_EQ(info.bounds.minX, -2.0f, 1e-3f);
    CHECK_FLOAT_EQ(info.bounds.maxY, 102.0f, 1e-3f);

    backend.releasePath(path);
    CHECK_FALSE(backend.getPathDebugInfo(path, info));
}

TEST_CASE(vector_path_curves_use_adaptive_segments_and_mixed_contours) {
    UIRenderBackend backend;
    const auto path = backend.createPath();
    backend.addPathRoundedRect(path, FRectangle(10, 10, 210, 110), 18.0f);
    backend.addPathEllipse(path, FVector2(320, 80), 60.0f, 35.0f);
    backend.addPathLine(path, FVector2(0, 180), FVector2(100, 220));
    backend.addPathBezier(path, FVector2(120, 200), FVector2(170, 120),
                          FVector2(240, 280), FVector2(300, 200));
    backend.addPathArc(path, FVector2(390, 200), 55.0f, 0.0f, 3.14159265f);
    backend.setPathStrokeWidth(path, 3.0f);

    UIRenderBackend::PathDebugInfo info;
    CHECK(backend.getPathDebugInfo(path, info));
    CHECK(info.contourCount == 5u);
    CHECK(info.openContourCount == 3u);
    CHECK(info.fillTriangleCount > 20u);
    CHECK(info.strokeTriangleCount > 40u);
    CHECK(info.bounds.minX < 10.0f);
    CHECK(info.bounds.maxX > 440.0f);
    CHECK(info.bounds.maxY > 220.0f);
}

TEST_CASE(vector_path_clockwise_hole_and_nested_clip_emit_ordering_barriers) {
    UIRenderBackend backend;
    backend.setFramebufferSize(640, 480);
    backend.beginFrame();

    const auto path = backend.createPath();
    backend.addPathRect(path, FRectangle(10, 10, 300, 260),
                        PathWinding::CounterClockwise);
    backend.addPathRect(path, FRectangle(80, 70, 180, 170),
                        PathWinding::Clockwise);

    UIRenderBackend::PathDebugInfo info;
    CHECK(backend.getPathDebugInfo(path, info));
    CHECK(info.contourCount == 2u);
    CHECK(info.clockwiseContourCount == 1u);
    CHECK(info.fillTriangleCount == 4u);

    backend.pushPathClip(path);
    CHECK(backend.getActivePathClipDepthForDebug() == 1u);
    backend.drawRect(FRectangle(0, 0, 400, 300), FVector4(1, 0, 0, 1));
    backend.pushPathClip(path);
    CHECK(backend.getActivePathClipDepthForDebug() == 2u);
    backend.drawPath(path, PathFillMode::FillAndStroke);
    backend.releasePath(path); // pending commands own immutable snapshots
    backend.popClip();
    CHECK(backend.getActivePathClipDepthForDebug() == 1u);
    backend.popClip();
    CHECK(backend.getActivePathClipDepthForDebug() == 0u);
    CHECK(backend.hasPathOrderingBarrierForDebug());
    CHECK(backend.getPendingUiItemCountForDebug() == 7u);

    backend.endFrame(); // uninitialized backend safely discards CPU commands
    CHECK(backend.getPendingUiItemCountForDebug() == 0u);
}

TEST_CASE(vector_path_barrier_only_splits_neighboring_batch_segments) {
    VectorPathHarness optimized;
    CHECK(optimized.init());
    optimized.ui.setBatchMode(UIRenderBackend::BatchMode::OverlapAware);
    drawSegmentedBarrierFrame(optimized.ui);
    CHECK(optimized.ui.getDrawCallCount() == 7);

    VectorPathHarness ordered;
    CHECK(ordered.init());
    ordered.ui.setBatchMode(UIRenderBackend::BatchMode::OrderedRuns);
    drawSegmentedBarrierFrame(ordered.ui);
    CHECK(ordered.ui.getDrawCallCount() == 9);
}

TEST_SUITE_END
