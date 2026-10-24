#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "AYRenderer.h"
#include "AYRenderer/PbrShaderSources.h"
#include "ProceduralGalleryGeometry.h"
#include "detail/TextureImageLoader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <array>
using namespace ayt::render;
using namespace ayt::math;
namespace fs=std::filesystem;
void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(int argc,char** argv){try{
 require(argc>=3&&argc<=5,"Usage: CloudRenderTest <fresh-output-directory> d3d11|d3d12 [imported-cloud-asset-directory] [four-pack-lab-directory]");const fs::path output=fs::absolute(argv[1]);require(!fs::exists(output),"Use fresh output directory");fs::create_directories(output);
 const std::string backend=argv[2];require(backend=="d3d11"||backend=="d3d12","Select D3D backend");
 WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"AYCloudRenderTest";RegisterClassW(&wc);auto window=CreateWindowW(wc.lpszClassName,L"Cloud acceptance",WS_OVERLAPPEDWINDOW,0,0,400,240,nullptr,nullptr,wc.hInstance,nullptr);require(window!=nullptr,"Create hidden native window");
 Renderer renderer;InitDesc init;init.windowHandle=window;init.width=320;init.height=180;init.msaa=0;init.vsync=false;init.backend=backend=="d3d11"?Backend::Direct3D11:Backend::Direct3D12;require(renderer.initialize(init),"Initialize actual device");
 renderer.configurePipeline(RenderPipelineDesc::makeDeferred());renderer.setShadowsEnabled(false);renderer.setFxaaEnabled(false);renderer.setPostProcessGamma(1);renderer.setPostProcessTonemapMode(Renderer::TonemapMode::None);
 renderer.setMainCameraLookAtPerspective({0,2,-8},{0,4,4},{0,1,0},60,320.f/180,.1f,120);
 auto cube=ayt::render::demo::createGalleryCube(renderer);auto material=renderer.createMaterialFromPhoskia(kPbrPhoskiaSource);renderer.setMaterialColor(material,"baseColor",.8f,.1f,.05f,1);renderer.setMaterialModel(material,MaterialModel::Unlit);
 const uint8_t white[]={255,255,255,255},black[]={0,0,0,255};auto whiteMap=renderer.createTextureFromRgba8(1,1,white,"cloud-test-white"),blackMap=renderer.createTextureFromRgba8(1,1,black,"cloud-test-black");renderer.setMaterialTexture(material,"baseColorTexture",whiteMap);
 std::vector<uint8_t> atlas(272*136*4,255),emptyAtlas(272*136*4,0);auto noise=renderer.createTextureFromRgba8(272,136,atlas.data(),"cloud-test-solid-atlas"),zeroNoise=renderer.createTextureFromRgba8(272,136,emptyAtlas.data(),"cloud-test-empty-atlas");require(cube.isValid()&&material.isValid()&&noise.isValid(),"Create test assets");
 RenderScene scene;scene.add(cube,material,translate(0,2,0)*scale(3,4,2));SceneLights lights;lights.add(Light::directional({0,-1,0},{1,1,1}));scene.setSceneLights(lights);
 auto capture=[&](const char* name,CloudSettings cloud,bool submitted){renderer.setCloudSettings(cloud);auto path=output/(std::string(name)+".png");
  for(int i=0;i<12;++i){ClearDesc clear;clear.r=.2f;clear.g=.4f;clear.b=.7f;renderer.beginFrame(clear);renderer.render(scene);if(i==5)require(renderer.captureScreenshot(path.string()),"Queue capture");renderer.endFrame();
   const auto& passes=renderer.getFrameStats().passes;auto it=std::find_if(passes.begin(),passes.end(),[](const auto& p){return p.name=="Clouds";});require((it!=passes.end()&&it->drawCalls==1)==submitted,"Cloud GPU dispatch eligibility");}
  auto image=ayt::render::detail::decodeImageFile(path.string());require(image.isValid(),"Read actual captured pixels");return image;};
 const auto difference=[](const auto& a,const auto& b,int x0,int y0,int x1,int y1){double total=0;for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)for(int c=0;c<3;++c){size_t i=(size_t(y)*a.width+x)*4+c;total+=std::abs(int(a.rgba8[i])-int(b.rgba8[i]));}return total/((x1-x0)*(y1-y0)*3);};
 CloudSettings s;auto base=capture("disabled",s,false);s.enabled=true;s.coverage=1;s.density=1;s.baseHeight=6;s.thickness=4;s.absorption=.5f;s.scattering=0;s.shapeNoise=noise;s.detailNoise=noise;s.erosion=0;s.quality.maxDistance=200;s.quality.steps=32;
 auto opaque=capture("absorbing-cloud",s,true);double sky=difference(base,opaque,5,5,315,45),foreground=difference(base,opaque,145,83,175,97);require(sky>20,"Cloud must attenuate background sky");require(foreground<.5,"Foreground geometry before cloud must remain unmodified");
 s.shapeNoise=zeroNoise;auto empty=capture("empty-density",s,true);require(difference(base,empty,0,0,320,180)<.5,"Empty density atlas must be transparent");s.shapeNoise=noise;s.weatherMap=blackMap;auto dry=capture("zero-weather-map",s,true);require(difference(base,dry,0,0,320,180)<.5,"Zero weather map must remove coverage");
 s.weatherMap=whiteMap;s.scattering=1;s.anisotropy=0;s.ambientColor={0,0,0};s.shadowStrength=0;auto lit=capture("unshadowed",s,true);s.shadowStrength=1;auto shaded=capture("self-shadowed",s,true);require(difference(lit,shaded,5,5,315,45)>5,"Light march must produce cloud self shadow");
 s.shapeNoise={};s.detailNoise={};s.coverage=.65f;s.shapeScale=16;s.detailScale=2;s.scattering=.5f;s.shapePhase={0,0,0};auto first=capture("procedural",s,true);s.shapePhase={.33f,.17f,.25f};auto moved=capture("advected",s,true);require(difference(first,moved,5,5,315,45)>1,"Cloud phase must move three-dimensional density");
 s.shapePhase={0,0,0};s.quality.samplingPhase=.37f;auto resampled=capture("sample-phase",s,true);require(difference(first,resampled,5,5,315,45)>.1,"Integration phase must change quadrature without changing cloud state");s.quality.samplingPhase=1;auto sampleWrap=capture("sample-phase-wrap",s,true);require(difference(first,sampleWrap,0,0,320,180)<.5,"Sampling phase wraps without changing density or geometry");s.quality.samplingPhase=0;
 s.shapePhase={0,0,0};auto wrap0=capture("phase-zero",s,true);s.shapePhase={1,1,1};auto wrap1=capture("phase-one",s,true);require(difference(wrap0,wrap1,0,0,320,180)<.5,"Wrapped phase must preserve density including domain warp");
 s.enabled=false;auto stopped=capture("disabled-after-enabled",s,false);require(difference(base,stopped,0,0,320,180)<.5,"Disable must not reuse stale cloud output");
 renderer.resize(400,200);renderer.setViewportRect(0,0,400,200);renderer.setMainCameraLookAtPerspective({0,2,-8},{0,4,4},{0,1,0},60,2,.1f,120);s.enabled=true;auto resized=capture("resized",s,true);require(resized.width==400&&resized.height==200,"Cloud pass survives target resize");for(size_t i=3;i<resized.rgba8.size();i+=4)require(resized.rgba8[i]==255,"Resize fixture must render the entire updated viewport");
 renderer.setMainCameraLookAtPerspective({8000,700,-12000},{0,2300,6000},{0,1,0},65,2,.1f,120);s.baseHeight=1500;s.thickness=3000;s.shapeScale=8000;s.detailScale=350;s.absorption=.002f;s.quality.maxDistance=60000;s.coverage=.6f;s.weatherMap={};
 auto distant=capture("kilometre-camera",s,true);require(distant.isValid(),"Camera-relative cloud rays work away from world origin");
 std::array<TextureHandle,2> imported{};
 if(argc>=4){const fs::path assets=argv[3];imported[0]=renderer.createTextureFromFile((assets/"BaseNoise.png").string());imported[1]=renderer.createTextureFromFile((assets/"DetailNoise.png").string());require(imported[0].isValid()&&imported[1].isValid(),"Load actual imported linear 128/32 volume atlases");}
 // Structural image checks complement the gallery's visual review: both clear sky and opaque
 // bodies must exist from different elevations, with varying cloud tops rather than a flat band.
 scene=RenderScene{};scene.setSceneLights(lights);s=CloudSettings{};s.enabled=true;s.coverage=.55f;s.density=.6f;s.thickness=3000;s.shapeScale=8000;s.detailScale=350;s.absorption=.002f;s.quality.steps=96;s.quality.debugView=1;s.shapeNoise=imported[0];s.detailNoise=imported[1];
 auto morphology=[&](const char* name){auto image=capture(name,s,true);size_t clear=0,thick=0;int minTop=int(image.height),maxTop=-1;
  for(uint32_t x=0;x<image.width;++x){int top=-1;for(uint32_t y=0;y<image.height;++y){auto value=image.rgba8[(size_t(y)*image.width+x)*4];clear+=value>240;thick+=value<64;if(value<64&&top<0)top=int(y);}if(top>=0){minTop=std::min(minTop,top);maxTop=std::max(maxTop,top);}}
  const double pixels=double(image.width)*image.height;std::cout<<name<<" clear="<<clear/pixels<<" thick="<<thick/pixels<<" topVariation="<<maxTop-minTop<<'\n';require(clear/pixels>.15,"Broken clouds must leave large clear-sky regions");require(thick/pixels>.01,"Broken clouds must include optically thick three-dimensional bodies");require(maxTop-minTop>10,"Cloud tops must vary rather than form a uniform horizontal sheet");};
 renderer.setMainCameraLookAtPerspective({8000,700,-12000},{0,2300,6000},{0,1,0},65,2,.1f,120);morphology("cumulus-below");renderer.setMainCameraLookAtPerspective({0,6500,-8000},{0,1800,10000},{0,1,0},65,2,.1f,120);morphology("cumulus-above");
 s.weatherMap=whiteMap;s.coverage=.45f;s.cloudType=0;auto layer=capture("same-coverage-layer",s,true);s.cloudType=1;auto towers=capture("same-coverage-cumulus",s,true);require(difference(layer,towers,0,0,400,200)>5,"Cloud type must change shape without changing coverage");
 s.edgeSoftness=0;auto hard=capture("edge-hard",s,true);s.edgeSoftness=1;auto soft=capture("edge-soft",s,true);const auto partial=[](const auto& image){size_t count=0;for(size_t i=0;i<image.rgba8.size();i+=4)count+=image.rgba8[i]>239&&image.rgba8[i]<255;return count;};std::cout<<"thin edge rim hard="<<partial(hard)<<" soft="<<partial(soft)<<'\n';require(difference(hard,soft,0,0,400,200)>1&&partial(soft)>partial(hard)*1.5,"Soft edges must add low-density, nearly transparent cloud rim pixels");
 const uint8_t dryField[]={0,0,0,255},rainField[]={0,0,255,255},layerField[]={255,0,0,128};auto dryTex=renderer.createTextureFromRgba8(1,1,dryField,"field-dry"),rainTex=renderer.createTextureFromRgba8(1,1,rainField,"field-rain"),layerTex=renderer.createTextureFromRgba8(1,1,layerField,"field-layer");
 s.spatialWeather=true;s.weatherMap=layerTex;s.coverage=1;auto packedLayer=capture("packed-field-layer",s,true);require(difference(packedLayer,towers,0,0,400,200)>5,"Packed field must supply type and thickness");
 s.weatherOrigin={1e6f,1e6f};s.weatherExtent={100,100};auto outside=capture("outside-field",s,true);double outsideDark=0;for(size_t i=0;i<outside.rgba8.size();i+=4)outsideDark+=255-outside.rgba8[i];require(outsideDark/(400*200)<.5,"Nonrepeating field must be clear outside its bounds");s.weatherRepeat=true;auto repeated=capture("repeat-field",s,true);require(difference(repeated,outside,0,0,400,200)>5,"Explicit repeat must sample wrapped field");
 renderer.setMainCameraLookAtPerspective({0,600,-8000},{0,600,10000},{0,1,0},60,2,1,60000);s.weatherOrigin={-32000,-32000};s.weatherExtent={64000,64000};s.weatherRepeat=false;s.weatherMap=dryTex;auto dryShaft=capture("dry-shaft",s,true);s.weatherMap=rainTex;auto wetShaft=capture("rain-shaft",s,true);require(difference(dryShaft,wetShaft,0,0,400,200)>5,"Precipitation field must create distant rain extinction below cloud base");
 renderer.setMainCameraLookAtPerspective({0,2,-8},{0,4,4},{0,1,0},60,2,.1f,120);scene.add(cube,material,translate(0,2,0)*scale(3,4,2));s=CloudSettings{};auto atmosphereOff=capture("atmosphere-off",s,false);s.atmosphereEnabled=true;auto atmosphere=capture("atmosphere-only",s,true);require(difference(atmosphereOff,atmosphere,5,5,395,45)>5,"Atmosphere must work with cloud density disabled");require(difference(atmosphereOff,atmosphere,185,100,215,112)<.5,"Sky atmosphere must not refog foreground geometry");
 scene=RenderScene{};scene.setSceneLights(lights);renderer.setMainCameraLookAtPerspective({0,12,-32},{0,1200,5000},{0,1,0},58,2,1,60000);
 s=CloudSettings{};s.enabled=true;s.coverage=.7f;s.weatherMap=whiteMap;s.density=.4f;s.shapeScale=7000;s.thickness=2600;s.quality.steps=96;s.shapeNoise=imported[0];s.detailNoise=imported[1];
 s.form=CloudForm::Cumulus;auto formCumulus=capture("form-cumulus",s,true);s.form=CloudForm::Stratocumulus;auto formLayered=capture("form-layered",s,true);require(difference(formCumulus,formLayered,0,0,400,200)>2,"Explicit cloud families must change static structure at equal coverage");
 s.form=CloudForm::Cirrus;s.baseHeight=8500;s.thickness=900;s.shapeScale=20000;s.density=.12f;s.absorption=.0008f;auto formCirrus=capture("form-cirrus",s,true);require(difference(formLayered,formCirrus,0,0,400,200)>2,"Thin directional cloud family must differ from low layered clouds");
 s=CloudSettings{};auto highOff=capture("high-off",s,false);s.enabled=true;s.coverage=0;s.density=0;s.highClouds={1,.4f,8500,800,22000};s.shapeNoise=imported[0];s.detailNoise=imported[1];auto highOn=capture("high-only",s,true);require(difference(highOff,highOn,0,0,400,200)>.5,"High thin clouds must submit and render independently of primary volume");
 renderer.setMainCameraLookAtPerspective({0,2,-8},{0,4,4},{0,1,0},60,2,.1f,120);scene.add(cube,material,translate(0,2,0)*scale(3,4,2));s.highClouds.coverage=0;s.enabled=false;auto highForegroundOff=capture("high-foreground-off",s,false);s.highClouds.coverage=1;s.enabled=true;auto highForeground=capture("high-foreground",s,true);require(difference(highForegroundOff,highForeground,185,100,215,112)<.5,"High clouds respect foreground geometry depth");
 // Opt-in actual asset matrix: the explicit lab argument is required for this acceptance run.
 if(argc==5){
  renderer.setMainCameraLookAtPerspective({0,12,-32},{0,1400,6000},{0,1,0},58,2,1,60000);
  scene=RenderScene{};SceneLights hybridLights;hybridLights.add(Light::directional({-.8f,-.6f,0},{1,1,1}));scene.setSceneLights(hybridLights);
  s=CloudSettings{};s.enabled=true;s.coverage=.85f;s.density=.45f;s.baseHeight=1600;s.thickness=2400;s.shapeScale=6500;s.quality.steps=96;s.quality.lightSteps=6;s.quality.debugView=1;s.weatherMap=whiteMap;s.form=CloudForm::Cumulus;s.structure.blend=1;
  std::array<std::array<TextureHandle,2>,4> packs{};decltype(base) firstPack;
  const char* packNames[]={"yangrc","lague-test","lague-flight","procedural"};
  for(int n=0;n<4;++n){for(int c=0;c<2;++c){packs[n][c]=renderer.createTextureFromFile((fs::path(argv[4])/packNames[n]/(c?"DetailNoise.png":"BaseNoise.png")).string());require(packs[n][c].isValid(),"Load mandatory packed cloud lab atlases");}
   s.shapeNoise=packs[n][0];s.detailNoise=packs[n][1];const auto name=std::string("hybrid-")+packNames[n];auto image=capture(name.c_str(),s,true);size_t opaquePixels=0;for(size_t i=0;i<image.rgba8.size();i+=4)opaquePixels+=image.rgba8[i]<64;require(opaquePixels>100,"Each noise pack must produce an optically thick body");
   if(n==0)firstPack=image;else{const double delta=difference(firstPack,image,0,0,400,200);std::cout<<name<<" delta="<<delta<<'\n';require(delta>1,"Independent imported/generated noise packs must change density structure");}
  }
  s.shapeNoise=packs[0][0];s.detailNoise=packs[0][1];s.structure.cellular=1;s.structure.billow=0;auto cells=capture("hybrid-cellular",s,true);s.structure.cellular=0;s.structure.billow=1;auto billow=capture("hybrid-billow",s,true);const double mixDelta=difference(cells,billow,0,0,400,200);std::cout<<"hybrid algorithm delta="<<mixDelta<<'\n';require(mixDelta>.5,"Independent cellular and billow algorithms must change shape at fixed assets and coverage");
  s.structure.warp=.85f;auto warped=capture("hybrid-warped",s,true);require(difference(warped,billow,0,0,400,200)>.5,"Domain warp must change structure without changing coverage");
  s.shapePhase={0,0,0};auto hybridZero=capture("hybrid-phase-zero",s,true);s.shapePhase={1,1,1};auto hybridOne=capture("hybrid-phase-one",s,true);require(difference(hybridZero,hybridOne,0,0,400,200)<.5,"Hybrid scales and warp preserve normalized phase wrap");
  s.shapePhase={};s.quality.debugView=0;s.shadowStrength=0;auto hybridUnshadowed=capture("hybrid-no-shadow",s,true);s.shadowStrength=1;auto hybridShadowed=capture("hybrid-shadowed",s,true);require(difference(hybridUnshadowed,hybridShadowed,0,0,400,200)>1,"Hybrid multiple-order lighting must preserve density self shadowing");
  // Ground overcast attenuation and incident cloud illumination have independent inputs.
  SceneLights groundLight;groundLight.add(Light::directional({-.8f,-.6f,0},{.12f,.12f,.12f}));scene.setSceneLights(groundLight);
  auto attenuated=capture("cloud-light-ground-attenuated",s,true);s.light.overrideScene=true;s.light.direction={-.8f,-.6f,0};s.light.radiance={1,1,1};auto incident=capture("cloud-light-incident",s,true);
  require(difference(attenuated,incident,0,0,400,200)>2,"Incident light override must illuminate clouds independently of ground attenuation");
  require(scene.sceneLights()->lights[0].color.x==.12f,"Cloud illumination must not mutate scene lighting");
  s.light.radiance={0,0,0};auto noIncident=capture("cloud-light-zero",s,true);require(difference(incident,noIncident,0,0,400,200)>2,"Explicit zero incident light removes direct cloud scattering");
  s.light.overrideScene=false;auto sceneAgain=capture("cloud-light-scene-restored",s,true);require(difference(attenuated,sceneAgain,0,0,400,200)<.5,"Disabling light override restores scene illumination");
  for(auto& pack:packs)for(auto texture:pack)renderer.destroyTexture(texture);
  std::cout<<"PASS four-pack cloud algorithm matrix "<<backend<<'\n';
 }
 renderer.destroyTexture(dryTex);renderer.destroyTexture(rainTex);renderer.destroyTexture(layerTex);
 for(auto texture:imported)if(texture.isValid())renderer.destroyTexture(texture);
 renderer.destroyTexture(noise);renderer.destroyTexture(zeroNoise);renderer.destroyTexture(whiteMap);renderer.destroyTexture(blackMap);renderer.destroyMaterial(material);renderer.destroyMesh(cube);renderer.shutdown();DestroyWindow(window);
 std::ofstream report(output/"result.txt");report<<"PASS "<<backend<<" skyDelta="<<sky<<" foregroundDelta="<<foreground<<"\n";std::cout<<"PASS cloud GPU "<<backend<<" skyDelta="<<sky<<" foregroundDelta="<<foreground<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
