#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/BGFXAdapter.h"
#include "detail/RenderPassContract.h"
#include "detail/DeferredDecalPass.h"
#include <limits>
using namespace ayt::render;
using namespace ayt::render::detail;
TEST_SUITE(AYRenderer_DeferredDecal)
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
}
