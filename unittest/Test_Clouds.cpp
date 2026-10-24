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
TEST_CASE(morphology_and_atmosphere_have_independent_bounded_controls){
 CloudSettings s;s.coverage=.2f;s.cloudType=0;s.clustering=std::numeric_limits<float>::quiet_NaN();s.edgeSoftness=3;s.weatherScale=0;s.aerialDensity=std::numeric_limits<float>::infinity();s.weatherExtent={0,-1};s=sanitizeCloudSettings(s);
 CHECK(s.coverage==.2f);CHECK(s.cloudType==0);CHECK(s.clustering==.55f);CHECK(s.edgeSoftness==1);CHECK(s.weatherScale==1);CHECK(s.aerialDensity==.000015f);CHECK(s.weatherExtent.x==1);CHECK(s.weatherExtent.y==1);
 s.coverage=1;s.cloudType=1;s.atmosphereEnabled=true;s.enabled=false;auto a=sanitizeCloudSettings(s);CHECK(a.cloudType==1);CHECK(!a.enabled&&a.atmosphereEnabled);
 s.form=CloudForm(255);s.highClouds.coverage=2;s.highClouds.thickness=-1;s.highClouds.scale=std::numeric_limits<float>::quiet_NaN();a=sanitizeCloudSettings(s);CHECK(a.form==CloudForm::Blended);CHECK(a.highClouds.coverage==1);CHECK(a.highClouds.thickness==1);CHECK(a.highClouds.scale==22000);
 s.quality.samplingPhase=std::numeric_limits<float>::infinity();a=sanitizeCloudSettings(s);CHECK(a.quality.samplingPhase==0);
}
TEST_CASE(atlas_layouts_and_independent_weather_distribution){
 auto a=cloudAtlasLayout(272,136),b=cloudAtlasLayout(528,528),c=cloudAtlasLayout(2080,1040);
 CHECK(a.resolution==32);CHECK(a.columns==8);CHECK(a.rows==4);CHECK(b.resolution==64);CHECK(c.resolution==128);CHECK(c.columns==16);CHECK(c.rows==8);
 CHECK(cloudAtlasLayout(2080,1041).resolution==0);CHECK(cloudAtlasLayout(256,128).resolution==0);
 auto weather=makeCloudWeather(1);CHECK(weather==makeCloudWeather(1));CHECK(weather!=makeCloudWeather(2));CHECK(weather.size()==256*256*4);
 int clear=0,dense=0;bool channels=true;for(size_t i=0;i<weather.size();i+=4){clear+=weather[i]<64;dense+=weather[i]>191;channels&=weather[i]==weather[i+1]&&weather[i]==weather[i+2]&&weather[i+3]==255;}
 CHECK(channels);CHECK(clear>16000&&clear<17000);CHECK(dense>16000&&dense<17000);
}
TEST_CASE(projective_camera_inverse_reprojects_independent_points){
 auto view=ayt::math::lh::lookAt({8,5,-12},{0,10,20},{0,1,0});auto projection=ayt::math::lh::perspective(ayt::math::radians(50.f),960.f/540,.1f,120.f);auto vp=projection*view;ayt::math::Float4x4 inverse;CHECK(cloudInverse(vp,inverse));
 for(auto p:{ayt::math::FVector4{-1,1,1,1},ayt::math::FVector4{1,-1,0,1},ayt::math::FVector4{0,0,.5f,1}}){auto world=inverse*p;auto clip=vp*world;CHECK(std::abs(clip.x-p.x)<.001f);CHECK(std::abs(clip.y-p.y)<.001f);CHECK(std::abs(clip.z-p.z)<.001f);CHECK(std::abs(clip.w-p.w)<.001f);}
 // CloudPass removes the rigid camera translation before inversion, including kilometre scenes.
 for(auto eye:{ayt::math::FVector3{8000,700,-12000},ayt::math::FVector3{12000,2000,-6000}}){auto relative=ayt::math::lh::lookAt(eye,{0,2300,6000},{0,1,0});relative.row[0].w=relative.row[1].w=relative.row[2].w=0;CHECK(cloudInverse(projection*relative,inverse));}
 auto singular=vp;for(int c=0;c<4;++c)singular.ptr()[c]=0;CHECK(!cloudInverse(singular,inverse));singular=vp;singular.row[0].x=std::numeric_limits<float>::quiet_NaN();CHECK(!cloudInverse(singular,inverse));
}
TEST_CASE(settings_and_graph_lifecycle){
 Renderer renderer;CHECK(!renderer.cloudSettings().enabled);CloudSettings s;s.enabled=true;s.coverage=2;s.thickness=-1;s.quality.steps=255;s.quality.lightSteps=255;s.baseHeight=std::numeric_limits<float>::quiet_NaN();
 renderer.setCloudSettings(s);s=renderer.cloudSettings();CHECK(s.coverage==1);CHECK(s.thickness==1);CHECK(s.baseHeight==1500);CHECK(s.quality.steps==96);CHECK(s.quality.lightSteps==8);
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
