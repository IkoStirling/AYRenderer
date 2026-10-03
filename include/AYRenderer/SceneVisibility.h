#pragma once

#include "AYRenderer/RenderScene.h"

namespace ayt::render {

// Presentation-only visibility used by editor and diagnostic viewports.
// Scene builders always produce the complete RenderScene; filtering copies
// the accepted DrawItems into a short-lived packet immediately before render.
// This keeps authoring data and Play/runtime behavior untouched.
struct SceneVisibilityFilter {
    bool meshes = true;
    bool worldLit2D = true;
    bool cameraOverlay2D = true;

    [[nodiscard]] constexpr bool allVisible() const noexcept {
        return meshes && worldLit2D && cameraOverlay2D;
    }
};

[[nodiscard]] inline bool isSceneItemVisible(
    const DrawItem& item,
    const SceneVisibilityFilter& filter) noexcept
{
    if (item.payload == nullptr) return filter.meshes;
    return item.payload->renderDomain == RenderDomain2D::WorldLit
        ? filter.worldLit2D : filter.cameraOverlay2D;
}

// The destination's ownership mode decides whether accepted draw data is
// borrowed or copied. An OwnedCpu destination may outlive the source's CPU
// storage; GPU stream/resource lifetimes still require an owner-thread fence.
inline void copyVisibleSceneForPresentation(
    const RenderScene& source,
    RenderScene& destination,
    const SceneVisibilityFilter& filter)
{
    destination.clear();
    if (filter.cameraOverlay2D && source.hasOverlayCamera2D()) {
        const OverlayCamera2D& camera = source.overlayCamera2D();
        destination.setOverlayCamera2D(
            camera.view, camera.projection, camera.layerMask);
    }
    for (const DrawItem& item : source.items()) {
        if (isSceneItemVisible(item, filter)) destination.add(item);
    }
}

} // namespace ayt::render
