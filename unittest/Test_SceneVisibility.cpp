#include "AYRenderer/SceneVisibility.h"
#include "AYTest.h"

using ayt::render::DrawItem;
using ayt::render::DrawPayload2D;
using ayt::render::RenderDomain2D;
using ayt::render::RenderScene;
using ayt::render::SceneVisibilityFilter;
using ayt::render::copyVisibleSceneForPresentation;

TEST_SUITE(RendererSceneVisibilityTests)

TEST_CASE(scene_visibility_filters_a_presentation_copy_only)
{
    DrawPayload2D overlay;
    overlay.renderDomain = RenderDomain2D::SceneOverlay;
    DrawPayload2D worldLit;
    worldLit.renderDomain = RenderDomain2D::WorldLit;

    RenderScene complete;
    complete.add(DrawItem{});
    DrawItem overlayItem;
    overlayItem.payload = &overlay;
    complete.add(overlayItem);
    DrawItem worldLitItem;
    worldLitItem.payload = &worldLit;
    complete.add(worldLitItem);
    complete.setOverlayCamera2D(
        ayt::math::Float4x4::identity(),
        ayt::math::Float4x4::identity(), 0x25u);

    SceneVisibilityFilter filter;
    filter.meshes = false;
    filter.cameraOverlay2D = false;
    filter.worldLit2D = true;
    RenderScene visible;
    copyVisibleSceneForPresentation(complete, visible, filter);

    CHECK_INT_EQ(static_cast<uint32_t>(complete.items().size()), 3u);
    CHECK_TRUE(complete.hasOverlayCamera2D());
    CHECK_INT_EQ(static_cast<uint32_t>(visible.items().size()), 1u);
    CHECK(visible.items().front().payload == &worldLit);
    CHECK_FALSE(visible.hasOverlayCamera2D());
}

TEST_CASE(scene_visibility_categories_are_independent)
{
    DrawPayload2D overlay;
    overlay.renderDomain = RenderDomain2D::SceneOverlay;
    DrawPayload2D worldLit;
    worldLit.renderDomain = RenderDomain2D::WorldLit;
    DrawItem mesh;
    DrawItem overlayItem;
    overlayItem.payload = &overlay;
    DrawItem worldLitItem;
    worldLitItem.payload = &worldLit;

    RenderScene complete;
    complete.add(mesh);
    complete.add(overlayItem);
    complete.add(worldLitItem);
    complete.setOverlayCamera2D(
        ayt::math::Float4x4::identity(),
        ayt::math::Float4x4::identity(), 0x1234u);

    const auto copyWith = [&](SceneVisibilityFilter filter) {
        RenderScene result;
        copyVisibleSceneForPresentation(complete, result, filter);
        return result;
    };

    RenderScene meshes = copyWith({true, false, false});
    CHECK_INT_EQ(static_cast<uint32_t>(meshes.items().size()), 1u);
    CHECK(meshes.items().front().payload == nullptr);
    CHECK_FALSE(meshes.hasOverlayCamera2D());

    RenderScene overlayOnly = copyWith({false, false, true});
    CHECK_INT_EQ(static_cast<uint32_t>(overlayOnly.items().size()), 1u);
    CHECK(overlayOnly.items().front().payload == &overlay);
    CHECK_TRUE(overlayOnly.hasOverlayCamera2D());
    CHECK_INT_EQ(overlayOnly.overlayCamera2D().layerMask, 0x1234u);
}

TEST_SUITE_END
