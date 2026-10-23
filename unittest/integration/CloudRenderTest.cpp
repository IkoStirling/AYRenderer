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
using namespace ayt::render;
using namespace ayt::math;
namespace fs=std::filesystem;
void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
int main(int argc,char** argv){try{
 require(argc==3,"Usage: CloudRenderTest <fresh-output-directory> d3d11|d3d12");const fs::path output=fs::absolute(argv[1]);require(!fs::exists(output),"Use fresh output directory");fs::create_directories(output);
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
 s.weatherMap={};s.scattering=1;s.anisotropy=0;s.ambientColor={0,0,0};s.shadowStrength=0;auto lit=capture("unshadowed",s,true);s.shadowStrength=1;auto shaded=capture("self-shadowed",s,true);require(difference(lit,shaded,5,5,315,45)>5,"Light march must produce cloud self shadow");
 s.shapeNoise={};s.detailNoise={};s.coverage=.65f;s.shapeScale=16;s.detailScale=2;s.scattering=.5f;s.shapePhase={0,0,0};auto first=capture("procedural",s,true);s.shapePhase={.33f,.17f,.25f};auto moved=capture("advected",s,true);require(difference(first,moved,5,5,315,45)>1,"Cloud phase must move three-dimensional density");
 s.enabled=false;auto stopped=capture("disabled-after-enabled",s,false);require(difference(base,stopped,0,0,320,180)<.5,"Disable must not reuse stale cloud output");
 renderer.resize(400,200);renderer.setMainCameraLookAtPerspective({0,2,-8},{0,4,4},{0,1,0},60,2,.1f,120);s.enabled=true;auto resized=capture("resized",s,true);require(resized.width==400&&resized.height==200,"Cloud pass survives target resize");
 renderer.destroyTexture(noise);renderer.destroyTexture(zeroNoise);renderer.destroyTexture(whiteMap);renderer.destroyTexture(blackMap);renderer.destroyMaterial(material);renderer.destroyMesh(cube);renderer.shutdown();DestroyWindow(window);
 std::ofstream report(output/"result.txt");report<<"PASS "<<backend<<" skyDelta="<<sky<<" foregroundDelta="<<foreground<<"\n";std::cout<<"PASS cloud GPU "<<backend<<" skyDelta="<<sky<<" foregroundDelta="<<foreground<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
