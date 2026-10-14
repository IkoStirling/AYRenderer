#pragma once
#include <AYRenderer/RenderScene.h>
#include <cstdint>

namespace ayt::render {
/// Renderer-owned color/depth output. Zero is the window backbuffer; nonzero handles
/// are acquired with createCameraOutput and invalid after release or renderer shutdown.
struct CameraOutputHandle { uint64_t id = 0; };
/// Independent runtime view, using the stable Forward 3D + transparent + Overlay2D path.
/// View IDs are host identities, not GPU view numbers. Borrow scene only during renderViews.
struct CameraRenderView {
    uint32_t id = 0;
    const RenderScene* scene = nullptr;
    math::Float4x4 view = math::Float4x4::identity();
    math::Float4x4 projection = math::Float4x4::identity();
    math::FVector3 position{};
    uint32_t layerMask = 0xFFFFFFFFu;
    uint16_t x = 0, y = 0, width = 1, height = 1;
    CameraOutputHandle output{};
    uint32_t clearRgba = 0x14171FFF;
};
/// Host viewport recipe. Camera components with matching viewportId are selected independently.
/// Output dimensions must match width/height; backbuffer rectangles must fit the client.
struct CameraViewportDesc {
    int32_t id = 0;
    uint16_t x = 0, y = 0, width = 1, height = 1;
    CameraOutputHandle output{};
};
inline constexpr uint32_t kMaximumCameraViews = 16;

/// Copy a scene with a 3D camera layer filter. Overlay2D retains its own layer contract.
/// Owned CPU backing remains shared; the output is published only after the copy succeeds.
inline void copyCameraVisibleScene(const RenderScene& source, RenderScene& output, uint32_t layerMask,
                                   bool runtimeView = false) {
    RenderScene result(source.drawDataOwnership());
    if (source.hasOverlayCamera2D()) {
        const auto& camera = source.overlayCamera2D();
        result.setOverlayCamera2D(camera.view,camera.projection,camera.layerMask);
    }
    for (const auto& item : source.items()) {
        if (!item.payload && !(item.cameraLayerMask & layerMask)) continue;
        auto copy = item;
        // Editor selection uses fixed editor-only views and belongs to the main editor viewport.
        if (runtimeView) copy.outlineHull = false;
        result.add(copy);
    }
    output = std::move(result);
}
}
