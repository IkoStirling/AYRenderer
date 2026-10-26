#pragma once
#include <AYRenderer/RenderScene.h>
#include <cstdint>
#include <vector>

namespace ayt::render {
/// Renderer-owned color/depth output. Zero is the window backbuffer; nonzero handles
/// are acquired with createCameraOutput and invalid after release or renderer shutdown.
struct CameraOutputHandle { uint64_t id = 0; };
struct CameraReadbackHandle { uint64_t id = 0; };
struct CameraReadbackFrame {
    uint16_t width = 0, height = 0;
    /// Premultiplied RGBA8 rows in backend texture order. Flip rows when
    /// originBottomLeft is true before passing to a top-left video encoder.
    bool originBottomLeft = false;
    std::vector<uint8_t> pixels;
};
enum class CameraReadbackStatus { Invalid, Pending, Ready };
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
    /// Requested color/depth clear also executes when opaque geometry is empty.
    uint32_t clearRgba = 0x14171FFF;
    uint32_t spaceRegionId = 0; // zero preserves unpartitioned legacy rendering
    /// Offscreen Sprite-only premultiplied-alpha output. Requires clearRgba=0
    /// and an owned output; rejects 3D, WorldLit2D and particle draws.
    bool transparent2DOutput = false;
};
/// Host viewport recipe. Camera components with matching viewportId are selected independently.
/// Output dimensions must match width/height; backbuffer rectangles must fit the client.
struct CameraViewportDesc {
    int32_t id = 0;
    uint16_t x = 0, y = 0, width = 1, height = 1;
    CameraOutputHandle output{};
    uint32_t spaceRegionId = 0;
};
inline constexpr uint32_t kMaximumCameraViews = 16;

/// Copy a scene with a 3D camera layer filter and optional space region.
/// Overlay2D retains its own layer contract, but explicit region views also
/// exclude payload draws with no matching region.
/// Owned CPU backing remains shared; the output is published only after the copy succeeds.
inline void copyCameraVisibleScene(const RenderScene& source, RenderScene& output, uint32_t layerMask,
                                   bool runtimeView = false, uint32_t spaceRegionId = 0) {
    RenderScene result(source.drawDataOwnership());
    if (source.sceneLights()) result.setSceneLights(*source.sceneLights());
    if (source.hasOverlayCamera2D()) {
        const auto& camera = source.overlayCamera2D();
        result.setOverlayCamera2D(camera.view,camera.projection,camera.layerMask);
    }
    for (const auto& item : source.items()) {
        if (!item.payload && !(item.cameraLayerMask & layerMask)) continue;
        if (spaceRegionId && item.spaceRegionId != spaceRegionId) continue;
        auto copy = item;
        // Editor selection uses fixed editor-only views and belongs to the main editor viewport.
        if (runtimeView) copy.outlineHull = false;
        result.add(copy);
    }
    output = std::move(result);
}
}
