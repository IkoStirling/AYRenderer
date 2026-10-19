#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/BGFXAdapter.h"
#include "detail/RenderPassContract.h"
#include "detail/VolumetricHistoryState.h"
#include "detail/VolumetricPass.h"
#include <AYRenderer/FixtureEmitters.h>
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
TEST_CASE(immutable_density_grid_trilinear_sampling_atlas_packing_and_binding_lifetime) {
 std::vector<uint8_t> data{0,40,60,100,80,120,140,180};auto profile=VolumetricDensityField::create(2,2,2,data);CHECK(bool(profile));if(!profile)return;
 CHECK(std::abs(profile->sample({0,0,0})-90.f/255)<1e-6f);CHECK(std::abs(profile->sample({.25f,-.5f,.5f})-100.f/255)<1e-6f);CHECK(profile->sample({1.01f,0,0})==0);CHECK(profile->sample({std::numeric_limits<float>::quiet_NaN(),0,0})==0);
 data[0]=255;CHECK(profile->sample({-1,-1,-1})==0);CHECK(!VolumetricDensityField::create(65,1,1,data));CHECK(!VolumetricDensityField::create(2,2,2,std::span<const uint8_t>(data).first(7)));
 std::vector<uint8_t> slices(9);for(int i=0;i<9;++i)slices[i]=uint8_t(i*16);const auto layered=VolumetricDensityField::create(1,1,9,slices);const auto atlas=layered->atlas();CHECK(layered->atlasWidth()==8);CHECK(layered->atlasHeight()==2);CHECK(atlas[7*4]==112);CHECK(atlas[8*4]==128);CHECK(std::abs(layered->sample({0,0,.875f})-120.f/255)<1e-6f);
 Renderer renderer;InitDesc init;init.backend=Backend::Noop;CHECK(renderer.initialize(init));auto texture=createVolumetricDensityTexture(renderer,*profile);CHECK(texture.isValid());CHECK(renderer.setVolumetricDensityField({profile,texture}));CHECK(renderer.volumetricSettings().local);
 auto settings=renderer.volumetricSettings();settings.local=false;renderer.setVolumetricSettings(settings);CHECK(renderer.volumetricSettings().local);
 CHECK(!renderer.setVolumetricDensityField({profile,{}}));CHECK(!renderer.setVolumetricDensityField({profile,TextureHandle{999999}}));CHECK(renderer.volumetricDensityField().texture.id==texture.id);
 renderer.destroyTexture(texture);CHECK(!renderer.volumetricDensityField().profile);CHECK(!renderer.volumetricSettings().local);CHECK(renderer.setVolumetricDensityField({}));renderer.shutdown();CHECK(!renderer.volumetricDensityField().texture.isValid());
}
TEST_CASE(visible_aperture_extinction_samples_the_same_immutable_density_field) {
 PerformanceLight light;light.id=1;light.light=Light::spot({0,0,.5f},{0,0,-1},10,5,.95f,.9f,{1,0,0});light.apertureRadius=.1f;
 VolumetricSettings medium;medium.enabled=true;medium.density=.2f;medium.center={0,0,.25f};medium.halfExtent={2,2,2};
 uint8_t value=0;const auto empty=VolumetricDensityField::create(1,1,1,std::span<const uint8_t>(&value,1));value=255;const auto full=VolumetricDensityField::create(1,1,1,std::span<const uint8_t>(&value,1));
 const auto identity=ayt::math::Float4x4::identity();ParticleDrawData clear,dense;FixtureEmitterStats stats;
 CHECK(buildFixtureEmitterBatch({&light,1},identity,identity,100,100,{},medium,clear,stats,nullptr,empty.get()));CHECK(clear.vertices.size()==4);
 CHECK(buildFixtureEmitterBatch({&light,1},identity,identity,100,100,{},medium,dense,stats,nullptr,full.get()));CHECK(dense.vertices.size()==4);
 CHECK(clear.vertices[0].r>0);CHECK(std::abs(dense.vertices[0].r/clear.vertices[0].r-std::exp(-.1f))<1e-5f);
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
