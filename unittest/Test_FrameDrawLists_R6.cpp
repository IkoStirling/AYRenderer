#include "AYTest.h"

#include "detail/FrameDrawLists.h"

#include <unordered_map>
#include <limits>
#include <AYMath/MathUtils.h>
#include "detail/MotionVectorPass.h"
#include "detail/RigidInstancing.h"
#include "detail/GBufferPass.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Lexer.h"
#include "AYShader/Parser.h"
#include "AYShader/Ir.h"
#include "AYRenderer/ShadowShaderSources.h"

using namespace ayt::render;
using namespace ayt::render::detail;

namespace {

DrawItem item(uint64_t mesh, uint64_t material, float z = 0.0f)
{
    DrawItem result;
    result.mesh.id = mesh;
    result.material.id = material;
    result.world(2, 3) = z;
    return result;
}

} // namespace

TEST_SUITE(AYRenderer_FrameDrawLists_R6)

TEST_CASE(rigid_instance_columns_preserve_affine_geometry_and_reject_deformation)
{
    GpuMesh mesh;
    DrawItem draw = item(1,1);
    draw.world(0,0) = -2; draw.world(1,1) = 3; draw.world(2,2) = 4;
    draw.world(0,1) = 0.5f;
    draw.world(0,3) = 7; draw.world(1,3) = 8; draw.world(2,3) = 9;
    RigidInstanceData data{};
    CHECK(packRigidInstance(draw, mesh, data));
    for (int r=0; r<3; ++r) {
        const float reconstructed = data.columns[0][r] + 2*data.columns[1][r]
            + 3*data.columns[2][r] + data.columns[r][3];
        const float expected = draw.world(r,0) + 2*draw.world(r,1)
            + 3*draw.world(r,2) + draw.world(r,3);
        CHECK(reconstructed == expected);
    }
    mesh.hasSkinWeights=true; CHECK_FALSE(packRigidInstance(draw,mesh,data));
    mesh.hasSkinWeights=false; mesh.hasMorphTargets=true;
    CHECK_FALSE(packRigidInstance(draw,mesh,data)); mesh.hasMorphTargets=false;
    draw.outlineHull=true; CHECK_FALSE(packRigidInstance(draw,mesh,data)); draw.outlineHull=false;
    draw.world(2,2)=0; CHECK_FALSE(packRigidInstance(draw,mesh,data));
    draw.world(2,2)=4; draw.world(3,0)=0.1f; CHECK_FALSE(packRigidInstance(draw,mesh,data));
    draw.world(3,0)=0; draw.world(1,3)=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(packRigidInstance(draw,mesh,data));
}

TEST_CASE(rigid_runs_do_not_reorder_or_merge_incompatible_submissions)
{
    GpuMesh mesh;
    std::vector<DrawItem> draws(5, item(1,1));
    draws[2].material.id=2;
    std::vector<const DrawItem*> list;
    for (const auto& draw : draws) list.push_back(&draw);
    std::vector<RigidInstanceData> data;
    BGFXAdapter unavailable;
    CHECK(unavailable.availableInstances(256,sizeof(RigidInstanceData))==0);
    CHECK_FALSE(unavailable.bindInstances(nullptr,6,sizeof(RigidInstanceData)));
    CHECK(collectRigidInstances(list,0,mesh,256,data)==2);
    CHECK(collectRigidInstances(list,3,mesh,1,data)==1);
    draws[1].firstIndex=3; CHECK(collectRigidInstances(list,0,mesh,256,data)==1);
    draws[1].firstIndex=0; draws[1].world(0,0)=-1;
    CHECK(collectRigidInstances(list,0,mesh,256,data)==1);
    draws[1].world(0,0)=1; draws[1].jointCount=1;
    CHECK(collectRigidInstances(list,0,mesh,256,data)==1);
}

TEST_CASE(builtin_instanced_variants_lower_through_phoskia)
{
    using namespace ayt::shader;
    using namespace ayt::shader::phoskia;
    for (bool shadow : {false,true}) {
        const std::string source = rigidInstancedSource(shadow
            ? kShadowCasterPhoskiaSource : kGBufferPhoskiaSourceCStr, shadow);
        Lexer lexer(source); std::vector<Token> tokens; lexer.tokenize(tokens);
        Parser parser(tokens); auto ast=parser.parse();
        ir::IRGenerator generator; auto ir=generator.generate(*ast);
        AYBGFXConverter converter; BGFXConvertResult result;
        converter.convertBGFX(ir,result);
        CHECK(result.success);
        if (!result.success || result.materialStages.empty()) continue;
        const auto& stages = result.materialStages.front();
        CHECK(stages.vertex.find("i_data0") != std::string::npos);
        CHECK(stages.vertex.find("u_viewProj") != std::string::npos);
        CHECK(stages.vertex.find("u_modelViewProj") == std::string::npos);
        CHECK(stages.fragment.find("i_data0") == std::string::npos);
    }
}

TEST_CASE(classifies_and_stably_sorts_all_render_domains_once)
{
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    materials[1].blendMode = BlendMode::Opaque;
    materials[2].blendMode = BlendMode::Alpha;

    DrawPayload2D overlayFar;
    overlayFar.packedSortKey = 20;
    DrawPayload2D overlayNear;
    overlayNear.packedSortKey = 10;
    DrawPayload2D worldLit;
    worldLit.renderDomain = RenderDomain2D::WorldLit;

    RenderScene scene;
    DrawItem opaque = item(1, 1);
    opaque.outlineHull = true;
    scene.add(opaque);
    scene.add(item(2, 2, 2.0f));
    scene.add(item(3, 2, 8.0f));
    DrawItem overlayA = item(4, 1);
    overlayA.payload = &overlayFar;
    scene.add(overlayA);
    DrawItem overlayB = item(5, 1);
    overlayB.payload = &overlayNear;
    scene.add(overlayB);
    DrawItem lit2D = item(6, 1);
    lit2D.payload = &worldLit;
    scene.add(lit2D);

    FrameContext frame;
    frame.cameraPosition = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
    FrameDrawLists lists;
    buildFrameDrawLists(scene, meshes, materials, frame, lists);

    CHECK(lists.opaque3D.size() == 1u);
    CHECK(lists.gbufferOpaque.size() == 2u);
    CHECK(lists.worldLit2D.size() == 1u);
    CHECK(lists.transparent3D.size() == 2u);
    CHECK(lists.transparent3D[0].item->world(2, 3) == 8.0f);
    CHECK(lists.overlay2D.size() == 2u);
    CHECK(lists.overlay2D[0]->payload->packedSortKey == 10u);
    CHECK(lists.selectionOutlines.size() == 1u);
    CHECK(lists.shadowCasters.size() == 2u);
}

TEST_CASE(rejects_missing_material_before_any_pass_consumes_the_item)
{
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    RenderScene scene;
    scene.add(item(1, 99));

    FrameContext frame;
    FrameDrawLists lists;
    buildFrameDrawLists(scene, meshes, materials, frame, lists);

    CHECK(lists.stats.sourceItems == 1u);
    CHECK(lists.stats.missingMaterials == 1u);
    CHECK(lists.opaque3D.empty());
    CHECK(lists.transparent3D.empty());
}

TEST_CASE(routes_gpu_world_particles_through_transparent_sorting)
{
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    ParticleDrawData batch;
    batch.world3D = true;
    batch.gpuStream = &batch;
    batch.submitGpu = [](void*, uint16_t) {};
    RenderScene scene;
    DrawItem particle;
    particle.particleBatch = &batch;
    particle.world(2, 3) = 8.0f;
    scene.add(particle);

    FrameContext frame;
    frame.cameraPosition = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
    FrameDrawLists lists;
    buildFrameDrawLists(scene, meshes, materials, frame, lists);

    CHECK(lists.transparent3D.size() == 1u);
    CHECK(lists.transparent3D[0].item->particleBatch == &batch);
    CHECK(lists.transparent3D[0].distanceSquared == 64.0f);
    CHECK(lists.overlay2D.empty());
    CHECK(lists.stats.invalidHandles == 0u);
}

TEST_CASE(static_frustum_culls_camera_geometry_but_preserves_offscreen_shadows)
{
    std::unordered_map<uint64_t, GpuMesh> meshes;
    meshes[1].localBoundsValid = true;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    materials[1].blendMode = BlendMode::Opaque;
    materials[2].blendMode = BlendMode::Alpha;
    RenderScene scene;
    scene.add(item(1, 1, 3));
    for (const auto& p : {ayt::math::FVector3{-10, 0, 3}, {10, 0, 3},
                          {0, -10, 3}, {0, 10, 3}, {0, 0, -3}, {0, 0, 15}}) {
        auto draw = item(1, 1, p.z);
        draw.world(0, 3) = p.x;
        draw.world(1, 3) = p.y;
        scene.add(draw);
    }
    scene.add(item(1, 2, 15));
    FrameContext frame;
    frame.frustumCulling = true;
    frame.cullingViewProjection = ayt::math::lh::perspective(
        ayt::math::radians(90.0f), 1.0f, 1.0f, 10.0f);
    FrameDrawLists lists;
    buildFrameDrawLists(scene, meshes, materials, frame, lists);
    CHECK(lists.opaque3D.size() == 1u);
    CHECK(lists.transparent3D.empty());
    CHECK(lists.shadowCasters.size() == 7u);
    CHECK(lists.shadowBoundsItems.size() == 8u);
    CHECK(lists.stats.frustumTested == 8u);
    CHECK(lists.stats.frustumCulled == 7u);
    frame.frustumCulling = false;
    buildFrameDrawLists(scene, meshes, materials, frame, lists);
    CHECK(lists.opaque3D.size() == 7u);
    CHECK(lists.transparent3D.size() == 1u);
}

TEST_CASE(static_bounds_are_conservative_at_near_plane_and_with_negative_nonuniform_scale)
{
    std::unordered_map<uint64_t, GpuMesh> meshes;
    meshes[1].localBoundsValid = true;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    materials[1].blendMode = BlendMode::Opaque;
    RenderScene scene;
    scene.add(item(1, 1, 1.0f)); // intersects near plane
    scene.add(item(1, 1, 0.0f)); // camera inside; corners straddle w=0
    auto scaled = item(1, 1, 3.0f);
    scaled.world(0, 0) = -8;
    scaled.world(1, 1) = 0.25f;
    scaled.world(0, 3) = 5;
    scene.add(scaled); // origin outside, extent intersects
    FrameContext frame;
    frame.frustumCulling = true;
    frame.cullingViewProjection = ayt::math::lh::perspective(
        ayt::math::radians(90.0f), 1.0f, 0.1f, 10.0f);
    FrameDrawLists lists;
    buildFrameDrawLists(scene, meshes, materials, frame, lists);
    CHECK(lists.opaque3D.size() == 3u);
}

TEST_CASE(unknown_or_deformed_bounds_fail_open_and_selection_never_uses_static_bounds)
{
    std::unordered_map<uint64_t, GpuMesh> meshes;
    meshes[1].localBoundsValid = true;
    meshes[2].localBoundsValid = true;
    meshes[2].hasSkinWeights = true;
    meshes[3].localBoundsValid = true;
    meshes[3].hasMorphTargets = true;
    meshes[4].localBoundsValid = true;
    meshes[4].localBoundsMin.x = std::numeric_limits<float>::quiet_NaN();
    std::unordered_map<uint64_t, GpuMaterial> materials;
    materials[1].blendMode = BlendMode::Opaque;
    RenderScene scene;
    for (uint64_t mesh : {2u, 3u, 4u, 5u}) scene.add(item(mesh, 1, 100));
    auto selected = item(1, 1, 100);
    selected.outlineHull = true;
    scene.add(selected);
    auto displaced = item(1, 1, 100);
    displaced.allowFrustumCulling = false;
    scene.add(displaced);
    auto invalid = item(1, 1, 100);
    invalid.world(0, 0) = std::numeric_limits<float>::infinity();
    scene.add(invalid);
    FrameContext frame;
    frame.frustumCulling = true;
    FrameDrawLists lists;
    buildFrameDrawLists(scene, meshes, materials, frame, lists);
    CHECK(lists.opaque3D.size() == 7u);
    CHECK(lists.selectionOutlines.size() == 1u);
    CHECK(lists.stats.frustumBypassed == 7u);
}

TEST_CASE(stable_frustum_margin_keeps_subpixel_edge_and_ignores_raster_jitter)
{
    std::unordered_map<uint64_t, GpuMesh> meshes;
    meshes[1].localBoundsValid = true;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    materials[1].blendMode = BlendMode::Opaque;
    RenderScene scene;
    auto edge = item(1, 1, 0.5f);
    edge.world(0, 3) = 1.501f;
    scene.add(edge);
    FrameContext frame;
    frame.frustumCulling = true;
    frame.cullingMarginX = 0.002f;
    FrameDrawLists lists;
    for (float phase : {-0.01f, 0.01f}) {
        frame.projection(0, 3) = phase;
        buildFrameDrawLists(scene, meshes, materials, frame, lists);
        CHECK(lists.opaque3D.size() == 1u);
    }
    frame.cullingMarginX = 0;
    buildFrameDrawLists(scene, meshes, materials, frame, lists);
    CHECK(lists.opaque3D.empty());
    // GL-style clip depth keeps z in [-w, w], unlike zero-to-one.
    RenderScene behind;
    behind.add(item(1, 1, -0.75f));
    frame.cullingHomogeneousDepth = true;
    buildFrameDrawLists(behind, meshes, materials, frame, lists);
    CHECK(lists.opaque3D.size() == 1u);
    frame.cullingHomogeneousDepth = false;
    buildFrameDrawLists(behind, meshes, materials, frame, lists);
    CHECK(lists.opaque3D.empty());
}

TEST_CASE(culled_object_reentry_does_not_reuse_nonconsecutive_motion_history)
{
    MotionHistoryCache history;
    const auto world = ayt::math::Float4x4::identity();
    history.commit(1, 1, world, nullptr, 0, 1);
    CHECK(history.find(1, 1, 0, 2) != nullptr);
    CHECK(history.find(1, 1, 0, 3) == nullptr);
}

TEST_CASE(debug_observations_report_actual_culling_without_removing_shadow_casters) {
    std::unordered_map<uint64_t,GpuMesh> meshes;meshes[1].localBoundsValid=true;
    std::unordered_map<uint64_t,GpuMaterial> materials;materials[1].blendMode=BlendMode::Opaque;
    RenderScene scene;auto draw=item(1,1,.5f);draw.sourceEntityId=42;draw.sourceWorld=123;scene.add(draw);
    draw.sourceEntityId=43;draw.world(0,3)=100;scene.add(draw);
    FrameContext frame;frame.frustumCulling=true;frame.collectDebugDraws=true;FrameDrawLists lists;
    buildFrameDrawLists(scene,meshes,materials,frame,lists);
    CHECK(lists.debugDraws.size()==2);CHECK(lists.debugDraws[0].visibility==DebugDrawVisibility::Visible);
    CHECK(lists.debugDraws[1].visibility==DebugDrawVisibility::FrustumCulled);CHECK(lists.debugDraws[1].world==123);
    CHECK(lists.debugDraws[1].boundsValid);CHECK(lists.debugDraws[1].corners[0].x==99.5f);
    CHECK(lists.shadowCasters.size()==2);CHECK(lists.opaque3D.size()==1);
    frame.collectDebugDraws=false;buildFrameDrawLists(scene,meshes,materials,frame,lists);CHECK(lists.debugDraws.empty());
}
TEST_SUITE_END
