// A small procedural color gallery. No external asset download is required.
#define UNICODE
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "AYRenderer.h"
#include "AYRenderer/ShadowShaderSources.h"
#include "AYMath/MathUtils.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
struct State { bool running=true; };
LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM key,LPARAM extra)
{
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_CLOSE || (message==WM_KEYDOWN && key==VK_ESCAPE)) {
        if(state)state->running=false;
        return 0;
    }
    return DefWindowProcW(window,message,key,extra);
}
// Face-separated positions/normals/UVs: the convenience textured cube has
// positions and UVs only, so a lit gallery supplies an explicit normal stream.
ayt::render::MeshHandle createGalleryCube(ayt::render::Renderer& renderer)
{
    const float points[6][4][3]={
        {{-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f}},
        {{-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}},
        {{.5f,-.5f,-.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{.5f,.5f,-.5f}},
        {{-.5f,-.5f,.5f},{-.5f,-.5f,-.5f},{-.5f,.5f,-.5f},{-.5f,.5f,.5f}},
        {{-.5f,.5f,-.5f},{.5f,.5f,-.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}},
        {{-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,-.5f,-.5f},{-.5f,-.5f,-.5f}}};
    const float normals[6][3]={{0,0,-1},{0,0,1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    struct Vertex {float x,y,z,nx,ny,nz,u,v;};
    std::array<Vertex,24> vertices{};std::array<uint16_t,36> indices{};
    const uint16_t flipped[6]={0,2,1,0,3,2},straight[6]={0,1,2,0,2,3};
    for(int face=0;face<6;++face) {
        for(int corner=0;corner<4;++corner) {
            auto* p=points[face][corner];auto* n=normals[face];
            vertices[face*4+corner]={p[0],p[1],p[2],n[0],n[1],n[2],
                corner==1 || corner==2 ? 1.0f : 0.0f,corner<2 ? 1.0f : 0.0f};
        }
        for(int j=0;j<6;++j)indices[face*6+j]=uint16_t(face*4+(face==1?straight[j]:flipped[j]));
    }
    return renderer.createMesh(vertices.data(),uint32_t(vertices.size()),
        ayt::render::VertexLayoutDesc::position3Normal3TexCoord2(),indices.data(),uint32_t(indices.size()));
}
const char* names[]={"Neutral","Pixel Retro","Low Color","Dither Retro"};
}

int main(int argc,char** argv)
{
    using namespace ayt::render;
    using namespace ayt::math;
    bool gallery=false,hidden=false;
    Backend backend=Backend::Direct3D11;
    std::filesystem::path output="color-stylization-captures";
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--gallery")gallery=true;
        else if(arg=="--hidden")hidden=true;
        else if(arg=="--d3d12")backend=Backend::Direct3D12;
        else if(arg=="--output" && i+1<argc)output=argv[++i];
        else {std::fprintf(stderr,"Usage: AYRenderer_ColorStylizationDemo [--gallery] [--hidden] [--d3d12] [--output directory]\n");return 2;}
    }
    std::filesystem::create_directories(output);
    constexpr uint16_t width=960,height=540;
    State state;
    WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"AYColorStylizationGallery";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&wc);
    RECT rectangle{0,0,width,height};AdjustWindowRect(&rectangle,WS_OVERLAPPEDWINDOW,FALSE);
    HWND window=CreateWindowW(wc.lpszClassName,L"Retro Color Gallery",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,
        nullptr,nullptr,wc.hInstance,nullptr);
    if(!window)return 1;
    SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&state));
    if(!hidden)ShowWindow(window,SW_SHOW);
    Renderer renderer;
    InitDesc init;init.windowHandle=window;init.width=width;init.height=height;
    init.msaa=0;init.vsync=!gallery;init.backend=backend;
    if(!renderer.initialize(init)){DestroyWindow(window);return 1;}
    renderer.setFxaaEnabled(false);renderer.setSmaaEnabled(false);
    renderer.setShadowsEnabled(false);
    renderer.setPostProcessGamma(2.2f);renderer.setPostProcessExposure(1);
    renderer.setDirectionalLight(FVector3(-.3f,-.8f,.5f),FVector3(1.2f,1.1f,1));
    renderer.setMainCameraLookAtPerspective({8,6,-11},{0,1.1f,1},{0,1,0},48,float(width)/height,.1f,100);
    auto cube=createGalleryCube(renderer);
    const uint8_t white[4]={255,255,255,255};
    auto whiteTexture=renderer.createTextureFromRgba8(1,1,white,"retro_gallery_white");
    std::vector<TextureHandle> textures{whiteTexture};
    std::vector<MaterialHandle> materials;
    auto material=[&](float r,float g,float b,TextureHandle texture) {
        auto value=renderer.createMaterialFromPhoskia(kSimpleLitShadowPhoskiaSource);
        renderer.setMaterialTexture(value,"albedoMap",texture);
        renderer.setMaterialColor(value,"baseColor",r,g,b,1);
        materials.push_back(value);return value;
    };
    auto floorMaterial=material(.14f,.22f,.32f,whiteTexture);
    const float palette[][3]={{.96f,.24f,.21f},{1,.61f,.16f},{.84f,.85f,.24f},
        {.16f,.68f,.51f},{.15f,.52f,.93f},{.59f,.29f,.8f}};
    std::array<MaterialHandle,6> colors{};
    for(int i=0;i<6;++i)colors[i]=material(palette[i][0],palette[i][1],palette[i][2],whiteTexture);
    std::vector<uint8_t> gradient(256*64*4);
    for(int y=0;y<64;++y)for(int x=0;x<256;++x) {
        auto offset=(y*256+x)*4;float t=x/255.0f;
        gradient[offset]=uint8_t(255*t);gradient[offset+1]=uint8_t(255*(1-t));
        gradient[offset+2]=uint8_t(80+140*y/63);gradient[offset+3]=255;
    }
    auto ramp=renderer.createTextureFromRgba8(256,64,gradient.data(),"retro_gallery_ramp");
    textures.push_back(ramp);auto rampMaterial=material(1,1,1,ramp);
    bool valid=cube.isValid() && whiteTexture.isValid() && ramp.isValid();
    for(auto value:materials)valid=valid && value.isValid();
    if(!valid){renderer.shutdown();DestroyWindow(window);return 1;}
    int preset=1;bool deferred=false,paused=false;uint32_t tick=0;int previousGallery=-1;int captureFrames=0;
    renderer.setColorStylizationSettings(colorStylizationPreset(ColorStylizationPreset::PixelRetro));
    const auto start=std::chrono::steady_clock::now();float time=0;
    std::array<bool,256> wasDown{};
    auto pressed=[&](int key) {bool down=(GetAsyncKeyState(key)&0x8000)!=0;bool edge=down&&!wasDown[key];wasDown[key]=down;return edge;};
    std::puts("0..3 preset | F Forward/Deferred | Space freeze | G color grading | F9 capture | Esc quit");
    int failures=0;
    while(state.running && (!gallery || tick<8*24)) {
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT)state.running=false;
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(!state.running)break;
        const int galleryIndex=int(tick/24);
        if(gallery && galleryIndex!=previousGallery) {
            previousGallery=galleryIndex;deferred=galleryIndex>=4;preset=galleryIndex%4;
            renderer.configurePipeline(deferred?RenderPipelineDesc::makeDeferred():RenderPipelineDesc::makeDefault());
            renderer.setShadowsEnabled(false);
            renderer.setColorStylizationSettings(colorStylizationPreset(static_cast<ColorStylizationPreset>(preset)));
        }
        if(!gallery && GetForegroundWindow()==window) {
            for(int i=0;i<4;++i)if(pressed('0'+i)) {
                preset=i;renderer.setColorStylizationSettings(colorStylizationPreset(static_cast<ColorStylizationPreset>(i)));
            }
            if(pressed('F')){deferred=!deferred;renderer.configurePipeline(deferred?RenderPipelineDesc::makeDeferred():RenderPipelineDesc::makeDefault());}
            if(pressed(VK_SPACE))paused=!paused;
            if(pressed('G')){renderer.setColorGradingPreset(ColorGradingPreset::Warm);renderer.setColorGradingEnabled(!renderer.colorGradingEnabled());}
        }
        if(gallery)time=.8f;
        else if(!paused)time=std::chrono::duration<float>(std::chrono::steady_clock::now()-start).count();
        RenderScene scene;
        scene.add(cube,floorMaterial,translate(0,-.2f,1)*scale(12,.25f,9));
        scene.add(cube,rampMaterial,translate(0,2.3f,4)*scale(10,3,.2f));
        for(int i=0;i<6;++i) {
            const float x=(i-2.5f)*1.65f;
            scene.add(cube,colors[i],translate(x,.35f,1)*scale(1.35f,.65f,1.35f));
            scene.add(cube,colors[i],translate(x,1.4f,1)*rotate(FVector3(0,1,0),time*.35f+i*.2f)*scale(.85f,1.1f,.85f));
            for(int j=0;j<4;++j)scene.add(cube,colors[i],translate(x,.15f+j*.2f,-1.7f+j*.28f)*scale(.9f,.15f,.22f));
        }
        ClearDesc clear;clear.r=.025f;clear.g=.04f;clear.b=.08f;
        renderer.beginFrame(clear);renderer.render(scene);
        if(!gallery && GetForegroundWindow()==window && pressed(VK_F9))captureFrames=7;
        bool capture=gallery ? tick%24>=15 && tick%24<=21 : captureFrames-->0;
        if(capture) {
            const auto base=output/((deferred?std::string("deferred-"):std::string("forward-"))+std::to_string(preset));
            if(!renderer.captureScreenshot(base.string()+".png"))++failures;
            std::printf("Capture %s\n",base.string().c_str());
        }
        if(gallery && tick%24>=8) {
            const auto& stats=renderer.getFrameStats();
            uint32_t draws=0;for(const auto& pass:stats.passes)if(pass.name=="ColorStylization")draws+=pass.drawCalls;
            if(!stats.graph.compileSucceeded || draws!=(preset==0?0u:1u)) {
                ++failures;std::fprintf(stderr,"Gallery graph/draw failure tick=%u draws=%u compiled=%d\n",tick,draws,int(stats.graph.compileSucceeded));
            }
        }
        renderer.endFrame();++tick;
        std::wstring title=L"Retro Color Gallery | ";title+=deferred?L"Deferred | ":L"Forward | ";
        for(const char* p=names[preset];*p;++p)title+=wchar_t(*p);
        title+=L" | 0..3 look, F pipeline, Space freeze, G grade, F9 save";
        SetWindowTextW(window,title.c_str());
    }
    if(gallery)for(const char* path:{"forward","deferred"})for(int i=0;i<4;++i)
        if(!std::filesystem::exists(output/(std::string(path)+"-"+std::to_string(i)+".png")))++failures;
    for(auto value:materials)renderer.destroyMaterial(value);
    for(auto value:textures)renderer.destroyTexture(value);
    renderer.destroyMesh(cube);renderer.shutdown();DestroyWindow(window);
    std::printf("Gallery completed, failures=%d\n",failures);
    return failures?1:0;
}
