#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/BGFXAdapter.h"
#include "detail/RenderPassContract.h"
#include "detail/VolumetricHistoryState.h"
#include "detail/VolumetricPass.h"
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
TEST_CASE(independent_history_rejects_cuts_settings_gaps_seek_and_invalid_resources) {
 Renderer r;r.resetVolumetricHistory();VolumetricSettings s;s.froxelHistoryWeight=100;r.setVolumetricSettings(s);CHECK(r.volumetricSettings().froxelHistoryWeight==.95f);
 s.froxelHistoryWeight=std::numeric_limits<float>::quiet_NaN();CHECK(sanitizeVolumetricSettings(s).froxelHistoryWeight==.85f);
 VolumetricHistoryState h;FrameContext a;a.volumetric.froxel=true;a.volumetric.froxelHistory=true;a.volumetric.noiseTime=1;
 const auto grid=makeVolumetricGridLayout(a.volumetric,64,64,16384);CHECK(!h.reusable(a,grid,2));h.commit(a,grid,1);CHECK(h.reusable(a,grid,2));CHECK(!h.reusable(a,grid,3));
 auto b=a;b.cameraPosition.x+=.1f;CHECK(h.reusable(b,grid,2));b.cameraPosition.x+=5;CHECK(!h.reusable(b,grid,2));
 b=a;b.projection.row[0].x+=.1f;CHECK(!h.reusable(b,grid,2));b=a;b.view.row[0].y+=.5f;CHECK(!h.reusable(b,grid,2));
 b=a;b.volumetric.noiseTime=.9f;CHECK(!h.reusable(b,grid,2));b.volumetric.noiseTime=1.3f;CHECK(!h.reusable(b,grid,2));b.volumetric.noiseTime=1.01f;CHECK(h.reusable(b,grid,2));
 b=a;b.volumetric.density*=2;CHECK(!h.reusable(b,grid,2));auto resized=grid;resized.width+=1;CHECK(!h.reusable(a,resized,2));h.invalidate();CHECK(!h.reusable(a,grid,2));
 BGFXAdapter adapter;VolumetricPass pass;pass.prepareHistory(adapter,a.volumetric,grid);CHECK(!BGFXAdapter::isValid(pass.historyRead()));CHECK(!BGFXAdapter::isValid(pass.historyWrite()));pass.destroyResources(adapter);
 FrameGraph graph(adapter);PostProcessGraphPlanInput in;in.width=64;in.height=64;in.sceneColor={7};in.gbuffer={8};in.volumetric=true;in.volumetricGrid=grid;in.volumetricHistoryRead={9};in.volumetricHistoryWrite={10};
 const auto result=buildPostProcessGraphPlan(graph,in);CHECK(result.compileSucceeded);CHECK(graph.resolve(FgResourceId::FroxelHistoryRead).idx==9);CHECK(graph.resolve(FgResourceId::FroxelHistoryWrite).idx==10);CHECK(static_cast<uint8_t>(FgResourceId::FroxelHistoryRead)==32);CHECK(static_cast<uint8_t>(FgResourceId::FroxelHistoryWrite)==33);
 in.volumetricHistoryRead=in.volumetricHistoryWrite=BGFX_INVALID_HANDLE;CHECK(buildPostProcessGraphPlan(graph,in).compileSucceeded);CHECK(!BGFXAdapter::isValid(graph.resolve(FgResourceId::FroxelHistoryRead)));CHECK(!BGFXAdapter::isValid(graph.resolve(FgResourceId::FroxelHistoryWrite)));
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
