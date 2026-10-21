#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/BGFXAdapter.h"
#include "detail/RenderPassContract.h"
#include "detail/DeferredDecalPass.h"
#include "detail/DeferredDecalVisibility.h"
#include <limits>
using namespace ayt::render;
using namespace ayt::render::detail;
TEST_SUITE(AYRenderer_DeferredDecal)
TEST_CASE(render_target_descriptor_queries_and_borrowed_shells_fail_closed_without_device_work) {
 BGFXAdapter adapter;
 CHECK(!adapter.supportsRenderTargetTexture2D(4,4,bgfx::TextureFormat::RGBA8));
 CHECK(!BGFXAdapter::isValid(adapter.createBorrowedFrameBuffer({})));
 BGFXInitParams init;init.backend=Backend::Noop;CHECK(adapter.initialize(init));
 const auto textures=bgfx::getStats()->numTextures;
 const auto framebuffers=bgfx::getStats()->numFrameBuffers;
 for(const auto extent:{0u,uint32_t(UINT16_MAX)+1,UINT32_MAX}) {
  CHECK(!adapter.supportsRenderTargetTexture2D(extent,4,bgfx::TextureFormat::RGBA8));
  CHECK(!adapter.supportsRenderTargetTexture2D(4,extent,bgfx::TextureFormat::RGBA8));
 }
 CHECK(!adapter.supportsRenderTargetTexture2D(4,4,bgfx::TextureFormat::Unknown));
 CHECK(!adapter.supportsRenderTargetTexture2D(4,4,bgfx::TextureFormat::UnknownDepth));
 CHECK(!adapter.supportsRenderTargetTexture2D(4,4,static_cast<bgfx::TextureFormat::Enum>(UINT32_MAX)));
 CHECK(!BGFXAdapter::isValid(adapter.createBorrowedFrameBuffer({})));
 const bgfx::TextureHandle invalid[]={BGFX_INVALID_HANDLE};
 CHECK(!BGFXAdapter::isValid(adapter.createBorrowedFrameBuffer(invalid)));
 CHECK(bgfx::getStats()->numTextures==textures);CHECK(bgfx::getStats()->numFrameBuffers==framebuffers);
 DeferredDecalPass pass;CHECK(!pass.prepare(adapter,4,4,bgfx::TextureHandle{1},31));
 adapter.shutdown();pass.destroyResources(adapter);
 CHECK(!BGFXAdapter::isValid(pass.output(1)));
}
TEST_CASE(host_copy_sanitizes_projectors_and_bounds_capacity) {
 Renderer r;CHECK(!r.deferredDecalsEnabled());DeferredDecal d;
 std::vector<DeferredDecal> source(20,d);r.setDeferredDecals(source);CHECK(r.deferredDecals().size()==16);
 source[0].opacity=0;CHECK(r.deferredDecals()[0].opacity==1);
 d.localToWorld.row[0].x=0;r.setDeferredDecals({d});CHECK(r.deferredDecals().empty());
 d.localToWorld=ayt::math::Float4x4::identity();d.localToWorld.row[0].x=-1;r.setDeferredDecals({d});CHECK(r.deferredDecals().empty());
 d.localToWorld=ayt::math::Float4x4::identity();d.localToWorld.row[0].y=.5f;r.setDeferredDecals({d});CHECK(r.deferredDecals().empty());
 d.localToWorld=ayt::math::Float4x4::identity();d.opacity=std::numeric_limits<float>::quiet_NaN();r.setDeferredDecals({d});CHECK(r.deferredDecals().empty());
 d.opacity=1;d.materialStrength=5;r.setDeferredDecals({d});CHECK(r.deferredDecals()[0].materialStrength==1);
}
TEST_CASE(graph_declares_replaced_gbuffer_and_contract_rejects_late_decals) {
 BGFXAdapter idle;DeferredDecalPass pass;
 CHECK(!pass.prepare(idle,64,32,bgfx::TextureHandle{2},1));
 CHECK(!BGFXAdapter::isValid(pass.output(1)));CHECK(!BGFXAdapter::isValid(pass.output(0)));
 BGFXAdapter a;FrameGraph graph(a);PostProcessGraphPlanInput in;in.width=64;in.height=32;in.finalLdr=true;in.sceneColor={7};in.gbuffer={8};in.baseGBuffer={9};in.decals=true;in.ssr=true;in.ssao=true;
 auto result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(graph.shouldExecute(RenderPassSlot::DeferredDecal));CHECK(graph.resolve(FgResourceId::GBufferAlbedo).idx==8);
 in.decals=false;result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(!graph.shouldExecute(RenderPassSlot::DeferredDecal));
 auto desc=RenderPipelineDesc::makeDeferred();CHECK(validateRenderPipelineContracts(desc.passes).valid());
 auto decal=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::DeferredDecal);auto light=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::Lighting);
 std::iter_swap(decal,light);CHECK(!validateRenderPipelineContracts(desc.passes).valid());
 CHECK(static_cast<uint8_t>(RenderPassSlot::DeferredDecal)==24);CHECK(static_cast<uint8_t>(FgResourceId::BaseGBuffer)==29);
}

TEST_CASE(projector_visibility_rejects_each_clip_plane_in_both_depth_conventions) {
    for (bool homogeneous : {false, true}) {
        FrameContext frame;
        frame.frustumCulling = true;
        frame.cullingHomogeneousDepth = homogeneous;
        // Both matrices describe x/y [-1,1], z [0,1] in world space.
        if (homogeneous) {
            frame.cullingViewProjection(2, 2) = 2;
            frame.cullingViewProjection(2, 3) = -1;
        }
        DeferredDecal inside;
        inside.localToWorld(0, 0) = .25f;
        inside.localToWorld(1, 1) = .25f;
        inside.localToWorld(2, 2) = .25f;
        inside.localToWorld(2, 3) = .5f;
        CHECK(!deferredDecalOutsideFrustum(inside, frame));
        for (int plane = 0; plane < 6; ++plane) {
            auto outside = inside;
            outside.localToWorld(plane / 2, 3) = plane % 2 ? 2.0f : -2.0f;
            CHECK(deferredDecalOutsideFrustum(outside, frame));
        }
        inside.localToWorld(0, 3) = 1.25f; // Exact face contact is retained.
        CHECK(!deferredDecalOutsideFrustum(inside, frame));
    }
}

TEST_CASE(projector_visibility_preserves_camera_inside_near_crossings_and_rotated_extents) {
    FrameContext frame;
    frame.frustumCulling = true;
    frame.cullingViewProjection = ayt::math::lh::perspective(
        ayt::math::radians(90.0f), 1, .1f, 10);
    DeferredDecal decal;
    CHECK(!deferredDecalOutsideFrustum(decal, frame)); // Camera inside, w crosses zero.
    decal.localToWorld(2, 3) = 1;
    CHECK(!deferredDecalOutsideFrustum(decal, frame)); // Near-plane intersection.
    decal.localToWorld(2, 3) = -3;
    CHECK(deferredDecalOutsideFrustum(decal, frame));
    decal.localToWorld(2, 3) = 12;
    CHECK(deferredDecalOutsideFrustum(decal, frame));

    // Positive nonuniform scale, 30-degree rotation; center outside right but
    // actual box intersects. Testing only the origin would lose this decal.
    decal.localToWorld = ayt::math::Float4x4::identity();
    constexpr float c = .8660254f, s = .5f;
    decal.localToWorld(0, 0) = 4 * c;
    decal.localToWorld(1, 0) = 4 * s;
    decal.localToWorld(0, 1) = -.25f * s;
    decal.localToWorld(1, 1) = .25f * c;
    decal.localToWorld(0, 3) = 4;
    decal.localToWorld(2, 3) = 3;
    CHECK(validDeferredDecalTransform(decal.localToWorld));
    CHECK(!deferredDecalOutsideFrustum(decal, frame));
    decal.localToWorld(0, 3) = 20;
    CHECK(deferredDecalOutsideFrustum(decal, frame));
    frame.cullingViewProjection = frame.cullingViewProjection
        * ayt::math::lh::lookAt({20, 0, 0}, {20, 0, 1}, {0, 1, 0});
    CHECK(!deferredDecalOutsideFrustum(decal, frame)); // Moving camera restores it.
}

TEST_CASE(projector_visibility_uses_stable_margin_and_fails_open_for_unknown_arithmetic) {
    FrameContext frame;
    frame.frustumCulling = true;
    DeferredDecal decal;
    decal.localToWorld(0, 0) = .001f;
    decal.localToWorld(0, 3) = 1.004f;
    CHECK(deferredDecalOutsideFrustum(decal, frame));
    frame.cullingMarginX = .004f; // Two pixels at width 1000.
    CHECK(!deferredDecalOutsideFrustum(decal, frame));
    frame.projection(0, 2) = .8f; // Raster jitter must not change visibility.
    CHECK(!deferredDecalOutsideFrustum(decal, frame));
    frame.cullingMarginX = 0;
    decal.localToWorld(0, 3) = 20;
    CHECK(deferredDecalOutsideFrustum(decal, frame));
    frame.cullingMarginX = std::numeric_limits<float>::infinity();
    CHECK(!deferredDecalOutsideFrustum(decal, frame));
    frame.cullingMarginX = 0;
    frame.cullingViewProjection(0, 0) = std::numeric_limits<float>::quiet_NaN();
    CHECK(!deferredDecalOutsideFrustum(decal, frame));
    frame.cullingViewProjection = ayt::math::Float4x4::identity();
    decal.localToWorld(3, 0) = 1.e-7f;
    CHECK(!deferredDecalOutsideFrustum(decal, frame)); // Accepted near-affine input.
    decal.localToWorld(3, 0) = 0;
    decal.localToWorld(0, 0) = std::numeric_limits<float>::max();
    CHECK(!deferredDecalOutsideFrustum(decal, frame)); // Corner math overflows or spans screen.
}

TEST_CASE(projector_visibility_retains_binary16_world_position_rounding_at_near_plane) {
    FrameContext frame;
    frame.frustumCulling = true;
    frame.cullingViewProjection = ayt::math::lh::perspective(
        ayt::math::radians(90.0f), 1, .1f, 10);
    DeferredDecal decal;
    for (int axis=0;axis<3;++axis) decal.localToWorld(axis,axis)=.001f;
    // A real receiver at near=.1 rounds to .0999755859375 in RGBA16F.
    // Its quantized position lies on this cube although the exact box does
    // not intersect the camera frustum. Rejecting it would change the image.
    decal.localToWorld(2,3)=.0999755859375f-.001f;
    CHECK(!deferredDecalOutsideFrustum(decal,frame));
    decal.localToWorld(2,3)=.095f;
    CHECK(deferredDecalOutsideFrustum(decal,frame));
}

TEST_CASE(projector_selection_is_bounded_stable_and_controls_graph_eligibility) {
    FrameContext frame;
    frame.frustumCulling = true;
    std::vector<DeferredDecal> decals(5);
    decals[0].localToWorld(0, 3) = 20;
    decals[2].localToWorld(1, 3) = 20;
    decals[4].localToWorld(0, 3) = -20;
    auto visible = selectDeferredDecals(decals, frame);
    CHECK(visible.count == 2);
    CHECK(visible.indices[0] == 1);
    CHECK(visible.indices[1] == 3);
    decals[1].localToWorld(0, 3) = 20;
    visible = selectDeferredDecals(decals, frame);
    CHECK(visible.count == 1);
    CHECK(visible.indices[0] == 3);
    decals[3].localToWorld(0, 3) = 20;
    visible = selectDeferredDecals(decals, frame);
    CHECK(visible.count == 0);
    BGFXAdapter adapter;
    FrameGraph graph(adapter);
    PostProcessGraphPlanInput input;
    input.width = input.height = 16;
    input.sceneColor = {7}; input.gbuffer = {8};
    input.decals = visible.count != 0;
    CHECK(buildPostProcessGraphPlan(graph, input).compileSucceeded);
    CHECK(!graph.shouldExecute(RenderPassSlot::DeferredDecal));
    frame.frustumCulling = false;
    visible = selectDeferredDecals(decals, frame);
    CHECK(visible.count == decals.size());
    for (size_t i = 0; i < visible.count; ++i) CHECK(visible.indices[i] == i);
    CHECK(selectDeferredDecals({}, frame).count == 0);
    decals.resize(kMaxDeferredDecals);
    CHECK(selectDeferredDecals(decals, frame).count == kMaxDeferredDecals);
    decals.emplace_back();
    CHECK(selectDeferredDecals(decals, frame).count == 0);
}
}
