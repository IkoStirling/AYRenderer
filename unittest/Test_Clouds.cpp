#include "AYTest.h"
#include "AYRenderer.h"
#include "detail/CloudNoise.h"
#include "detail/CloudRay.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/RenderPassContract.h"
#include "detail/RenderViewOrder.h"
#include <limits>
using namespace ayt::render;
using namespace ayt::render::detail;
TEST_SUITE(AYRenderer_Clouds)
TEST_CASE(projective_camera_inverse_reprojects_independent_points){
 auto view=ayt::math::lh::lookAt({8,5,-12},{0,10,20},{0,1,0});auto projection=ayt::math::lh::perspective(ayt::math::radians(50.f),960.f/540,.1f,120.f);auto vp=projection*view;ayt::math::Float4x4 inverse;CHECK(cloudInverse(vp,inverse));
 for(auto p:{ayt::math::FVector4{-1,1,1,1},ayt::math::FVector4{1,-1,0,1},ayt::math::FVector4{0,0,.5f,1}}){auto world=inverse*p;auto clip=vp*world;CHECK(std::abs(clip.x-p.x)<.001f);CHECK(std::abs(clip.y-p.y)<.001f);CHECK(std::abs(clip.z-p.z)<.001f);CHECK(std::abs(clip.w-p.w)<.001f);}
 auto singular=vp;for(int c=0;c<4;++c)singular.ptr()[c]=0;CHECK(!cloudInverse(singular,inverse));singular=vp;singular.row[0].x=std::numeric_limits<float>::quiet_NaN();CHECK(!cloudInverse(singular,inverse));
}
TEST_CASE(settings_and_graph_lifecycle){
 Renderer renderer;CHECK(!renderer.cloudSettings().enabled);CloudSettings s;s.enabled=true;s.coverage=2;s.thickness=-1;s.quality.steps=255;s.quality.lightSteps=255;s.baseHeight=std::numeric_limits<float>::quiet_NaN();
 renderer.setCloudSettings(s);s=renderer.cloudSettings();CHECK(s.coverage==1);CHECK(s.thickness==1);CHECK(s.baseHeight==1500);CHECK(s.quality.steps==64);CHECK(s.quality.lightSteps==8);
 BGFXAdapter adapter;FrameGraph graph(adapter);PostProcessGraphPlanInput in;in.width=64;in.height=32;in.finalLdr=true;in.sceneColor={7};in.gbuffer={8};in.clouds=true;in.volumetric=true;in.haze=true;
 auto plan=buildPostProcessGraphPlan(graph,in);CHECK(plan.compileSucceeded);CHECK(plan.hdrSceneSource==FgResourceId::CloudColor);CHECK(graph.shouldExecute(RenderPassSlot::Clouds));
 in.clouds=false;plan=buildPostProcessGraphPlan(graph,in);CHECK(plan.compileSucceeded);CHECK(plan.hdrSceneSource==FgResourceId::VolumetricColor);CHECK(!graph.shouldExecute(RenderPassSlot::Clouds));CHECK(!BGFXAdapter::isValid(graph.resolve(FgResourceId::CloudColor)));
 auto desc=RenderPipelineDesc::makeDeferred();CHECK(validateRenderPipelineContracts(desc.passes).valid());
 auto cloud=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::Clouds),post=std::find(desc.passes.begin(),desc.passes.end(),RenderPassSlot::PostProcess);std::iter_swap(cloud,post);CHECK(!validateRenderPipelineContracts(desc.passes).valid());
 CHECK(static_cast<uint8_t>(RenderPassSlot::Clouds)==29);CHECK(static_cast<uint8_t>(FgResourceId::CloudColor)==36);
 auto rank=[](int id){return std::find(kRenderViewOrder.begin(),kRenderViewOrder.end(),id)-kRenderViewOrder.begin();};CHECK(rank(33)<rank(54));CHECK(rank(54)<rank(9));CHECK(rank(54)<rank(15));
}
TEST_CASE(noise_atlas_is_seeded_and_has_wrapped_slice_borders){
 auto a=makeCloudNoise(17),b=makeCloudNoise(17),c=makeCloudNoise(18);CHECK(a==b);CHECK(a!=c);int low=255,high=0;bool borders=true,alpha=true;
 auto pixel=[&](int z,int x,int y){return a[((z/8*34+y)*cloudAtlasWidth+z%8*34+x)*4];};
 for(int z=0;z<32;++z)for(int t=1;t<=32;++t)borders&=pixel(z,0,t)==pixel(z,32,t)&&pixel(z,33,t)==pixel(z,1,t)&&pixel(z,t,0)==pixel(z,t,32)&&pixel(z,t,33)==pixel(z,t,1);
 for(size_t i=0;i<a.size();i+=4){low=std::min(low,int(a[i]));high=std::max(high,int(a[i]));alpha&=a[i+3]==255;}CHECK(borders);CHECK(alpha);CHECK(low<90);CHECK(high>170);
}
}
