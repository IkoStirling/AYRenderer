#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/BGFXAdapter.h"
#include "detail/RenderPassContract.h"
#include <limits>
using namespace ayt::render;
using namespace ayt::render::detail;
TEST_SUITE(AYRenderer_Volumetric)
TEST_CASE(reduced_scattering_is_optional_bounded_and_frame_graph_owned) {
 Renderer r;CHECK(r.volumetricSettings().downsample==1);VolumetricSettings s;s.downsample=3;r.setVolumetricSettings(s);CHECK(r.volumetricSettings().downsample==2);s.downsample=255;r.setVolumetricSettings(s);CHECK(r.volumetricSettings().downsample==4);
 BGFXAdapter a;FrameGraph graph(a);PostProcessGraphPlanInput in;in.width=65;in.height=33;in.finalLdr=true;in.sceneColor={7};in.gbuffer={8};in.volumetric=true;
 auto result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);const auto original=graph.stats().physicalTargets;
 in.volumetricDownsample=2;result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(graph.stats().physicalTargets==original+1);
 in.volumetricDownsample=4;result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(graph.stats().physicalTargets==original+1);
 in.volumetric=false;result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(!BGFXAdapter::isValid(graph.resolve(FgResourceId::VolumetricIntegration)));
 CHECK(static_cast<uint8_t>(FgResourceId::VolumetricIntegration)==30);
}
TEST_CASE(froxel_layout_caps_memory_and_atlas_axes_without_overflow) {
 VolumetricSettings s;CHECK(!s.froxel);s.froxel=true;
 const auto typical=makeVolumetricGridLayout(s,1920,828,16384);CHECK(typical.valid());CHECK(typical.width==240);CHECK(typical.height==104);CHECK(typical.cells==798720);CHECK(typical.atlasWidth==1920);CHECK(typical.atlasHeight==416);
 for(const auto size:{uint16_t(1),uint16_t(1080),uint16_t(65535)})for(const auto budget:{65536u,1048576u,4194304u}){s.froxelMaxCells=budget;s.froxelSlices=64;const auto grid=makeVolumetricGridLayout(s,size,1,256);CHECK(grid.valid());CHECK(grid.cells<=budget);CHECK(grid.atlasWidth<=256);CHECK(grid.atlasHeight<=256);CHECK(grid.cells==uint32_t(grid.width)*grid.height*grid.slices);}
 s.froxelMaxCells=65536;s.froxelSlices=64;const auto dense=makeVolumetricGridLayout(s,65535,65535,16384);CHECK(dense.valid());CHECK(dense.cells<=65536);CHECK(dense.cells>0);CHECK(dense.atlasWidth<=16384);CHECK(dense.atlasHeight<=16384);
 CHECK(!makeVolumetricGridLayout(s,0,100,16384).valid());CHECK(!makeVolumetricGridLayout(s,100,100,4).valid());
 s.froxelSlices=0;s.froxelTileSize=255;s.froxelMaxCells=0;s=sanitizeVolumetricSettings(s);CHECK(s.froxelSlices==16);CHECK(s.froxelTileSize==16);CHECK(s.froxelMaxCells==65536);
 BGFXAdapter a;FrameGraph graph(a);PostProcessGraphPlanInput in;in.width=65;in.height=33;in.finalLdr=true;in.sceneColor={7};in.gbuffer={8};in.volumetric=true;
 auto result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);const auto original=graph.stats().physicalTargets;
 s.froxel=true;in.volumetricGrid=makeVolumetricGridLayout(s,65,33,16384);result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(graph.stats().physicalTargets==original+2);
 in.volumetric=false;result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(!BGFXAdapter::isValid(graph.resolve(FgResourceId::FroxelField)));CHECK(static_cast<uint8_t>(FgResourceId::FroxelField)==31);
}
TEST_CASE(settings_graph_and_disabled_resources) {
 Renderer r;CHECK(!r.volumetricSettings().enabled);
 VolumetricSettings s;s.enabled=true;s.density=-1;s.scattering=3;s.steps=255;s.center.x=std::numeric_limits<float>::quiet_NaN();s.halfExtent.y=-2;
 s.noiseStrength=4;s.noiseScale=0;s.noiseTime=std::numeric_limits<float>::quiet_NaN();
 r.setVolumetricSettings(s);s=r.volumetricSettings();CHECK(s.density==0);CHECK(s.scattering==1);CHECK(s.steps==64);CHECK(s.center.x==0);CHECK(s.halfExtent.y==.01f);
 CHECK(s.noiseStrength==1);CHECK(s.noiseScale==.01f);CHECK(s.noiseTime==0);
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
