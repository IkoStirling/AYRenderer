#include "AYTest.h"

#include "detail/FrameDrawLists.h"

#include <unordered_map>

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

TEST_SUITE_END
