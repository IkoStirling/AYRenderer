#include "AYRenderer/RenderScene.h"
#include "AYTest.h"

#include "detail/BGFXAdapter.h"
#include "detail/Draw2D.h"
#include "detail/GpuResources.h"
#include "detail/ShadowMapResources.h"
#include "detail/ShadowCaster.h"

#include <unordered_map>

using ayt::render::DrawItem;
using ayt::render::RenderScene;
using ayt::render::ShadowFlags;
using ayt::render::castsShadow;
using ayt::render::kShadowCastAndReceive;
using ayt::render::makeShadowFlags;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::ShadowCaster;
using ayt::render::detail::ShadowMapResources;

TEST_SUITE(ShadowCaster)

TEST_CASE(shadow_flags_helpers_match_mesh_component_semantics)
{
    CHECK(castsShadow(kShadowCastAndReceive));
    CHECK(castsShadow(ShadowFlags::Cast));
    CHECK(castsShadow(makeShadowFlags(true, false)) == true);
    CHECK(castsShadow(makeShadowFlags(false, true)) == false);
    CHECK(castsShadow(ShadowFlags::Receive) == false);
    CHECK(castsShadow(ShadowFlags::None) == false);
}

TEST_CASE(draw_item_defaults_to_cast_and_receive)
{
    DrawItem item;
    CHECK(castsShadow(item.shadowFlags));
    CHECK(item.shadowFlags == kShadowCastAndReceive);
}

TEST_CASE(shadow_domain_excludes_overlay_but_accepts_world_lit_2d)
{
    DrawItem mesh3d;
    CHECK(ayt::render::detail::isShadowDomainItem(mesh3d));

    ayt::render::DrawPayload2D overlayPayload;
    DrawItem overlay;
    overlay.payload = &overlayPayload;
    CHECK_FALSE(ayt::render::detail::isShadowDomainItem(overlay));

    ayt::render::DrawPayload2D worldPayload;
    worldPayload.renderDomain = ayt::render::RenderDomain2D::WorldLit;
    DrawItem worldLit;
    worldLit.payload = &worldPayload;
    CHECK(ayt::render::detail::isShadowDomainItem(worldLit));
}

TEST_CASE(draw_casters_empty_scene_returns_zero)
{
    BGFXAdapter adapter;
    ShadowCaster caster;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;

    CHECK(caster.drawCasters(adapter,
                             1,
                             ShadowMapResources::casterDrawState(),
                             scene,
                             meshes,
                             textures,
                             materials) == 0);
}

TEST_SUITE_END
