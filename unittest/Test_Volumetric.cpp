#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/BGFXAdapter.h"
#include "detail/RenderPassContract.h"
#include <limits>
using namespace ayt::render;
using namespace ayt::render::detail;
TEST_SUITE(AYRenderer_Volumetric)
TEST_CASE(settings_graph_and_disabled_resources) {
 Renderer r;CHECK(!r.volumetricSettings().enabled);
 VolumetricSettings s;s.enabled=true;s.density=-1;s.scattering=3;s.steps=255;s.center.x=std::numeric_limits<float>::quiet_NaN();s.halfExtent.y=-2;
 r.setVolumetricSettings(s);s=r.volumetricSettings();CHECK(s.density==0);CHECK(s.scattering==1);CHECK(s.steps==64);CHECK(s.center.x==0);CHECK(s.halfExtent.y==.01f);
 BGFXAdapter a;FrameGraph graph(a);PostProcessGraphPlanInput in;in.width=64;in.height=32;in.finalLdr=true;in.sceneColor={7};in.gbuffer={8};in.volumetric=true;in.haze=true;in.ssr=true;
 auto result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(result.hdrSceneSource==FgResourceId::VolumetricColor);CHECK(graph.shouldExecute(RenderPassSlot::Volumetric));
 in.volumetric=false;result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(!graph.shouldExecute(RenderPassSlot::Volumetric));CHECK(!BGFXAdapter::isValid(graph.resolve(FgResourceId::VolumetricColor)));
 CHECK(validateRenderPipelineContracts(RenderPipelineDesc::makeDeferred().passes).valid());
 CHECK(static_cast<uint8_t>(RenderPassSlot::Volumetric)==23);CHECK(static_cast<uint8_t>(FgResourceId::VolumetricColor)==28);
 auto desc=RenderPipelineDesc::makeDeferred();
 auto fog=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::Volumetric);
 auto bloom=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::BloomExtract);
 std::iter_swap(fog,bloom);CHECK(!validateRenderPipelineContracts(desc.passes).valid());
}
}
