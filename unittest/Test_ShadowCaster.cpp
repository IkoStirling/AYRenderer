#include "AYRenderScene.h"
#include "AYTest.h"

#include "detail/BGFXAdapter.h"
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

TEST_CASE(draw_casters_empty_scene_returns_zero)
{
    BGFXAdapter adapter;
    ShadowCaster caster;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;

    CHECK(caster.drawCasters(adapter,
                             1,
                             ShadowMapResources::casterDrawState(),
                             scene,
                             meshes) == 0);
}

TEST_SUITE_END
