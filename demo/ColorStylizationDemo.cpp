// A small procedural color gallery. No external asset download is required.
#define UNICODE
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "AYRenderer.h"
#include "AYRenderer/PbrShaderSources.h"
#include "AYRenderer/ColorStylizationLook.h"
#include "AYMath/MathUtils.h"
#include "ProceduralGalleryGeometry.h"
#include "DemoUtf8Arguments.h"
#include "PalettePerformance.h"

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
    struct Vertex {float x,y,z,nx,ny,nz,tx,ty,tz,tw,u,v;};
    std::array<Vertex,24> vertices{};std::array<uint16_t,36> indices{};
    const uint16_t flipped[6]={0,2,1,0,3,2},straight[6]={0,1,2,0,2,3};
    for(int face=0;face<6;++face) {
        for(int corner=0;corner<4;++corner) {
            auto* p=points[face][corner];auto* n=normals[face];
            vertices[face*4+corner]={p[0],p[1],p[2],n[0],n[1],n[2],
                points[face][1][0]-points[face][0][0],
                points[face][1][1]-points[face][0][1],
                points[face][1][2]-points[face][0][2],face==1 ? -1.0f : 1.0f,
                corner==1 || corner==2 ? 1.0f : 0.0f,corner<2 ? 1.0f : 0.0f};
        }
        for(int j=0;j<6;++j)indices[face*6+j]=uint16_t(face*4+(face==1?straight[j]:flipped[j]));
    }
    ayt::render::VertexLayoutDesc layout;
    using namespace ayt::render;
    layout.add({VertexAttribute::Position,3,VertexComponentType::Float,false});
    layout.add({VertexAttribute::Normal,3,VertexComponentType::Float,false});
    layout.add({VertexAttribute::Tangent,4,VertexComponentType::Float,false});
    layout.add({VertexAttribute::TexCoord0,2,VertexComponentType::Float,false});
    return renderer.createMesh(vertices.data(),uint32_t(vertices.size()),
        layout,indices.data(),uint32_t(indices.size()));
}
const char* names[]={"Neutral","Pixel Retro","Low Color","Dither Retro","Pocket","Sunset","Noir","Comic","Engraving","Blueprint","Arcade","Broadcast","Afterimage","Stop Motion","Tile Refresh","Time Echo","Cel Ink","Cel Paint"};
constexpr int lookCount=ayt::render::kColorStylizationPresetCount;
static_assert(std::size(names)==lookCount);
const char* sceneNames[]={"gallery","city","character","workshop","observatory","canal"};
}

int runGallery(int argc,char** argv)
{
    using namespace ayt::render;
    using namespace ayt::math;
    bool gallery=false,motionGallery=false,animatePalette=false,hidden=false,writePresets=false;
    int sceneOverride=-1;
    std::string lookPath;
    Backend backend=Backend::Direct3D11;
    std::filesystem::path output="color-stylization-captures";
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--gallery")gallery=true;
        else if(arg=="--animate-palette")animatePalette=true;
        else if(arg=="--motion-gallery"){gallery=true;motionGallery=true;}
        else if(arg=="--scene" && i+1<argc) {
            const std::string value=argv[++i];sceneOverride=-1;
            if(value!="auto") {
                for(int j=0;j<6;++j)if(value==sceneNames[j])sceneOverride=j;
                if(sceneOverride<0){std::fprintf(stderr,"Unknown scene: %s\n",value.c_str());return 2;}
            }
        }
        else if(arg=="--look" && i+1<argc)lookPath=argv[++i];
        else if(arg=="--write-presets")writePresets=true;
        else if(arg=="--hidden")hidden=true;
        else if(arg=="--d3d12")backend=Backend::Direct3D12;
        else if(arg=="--output" && i+1<argc)output=argv[++i];
        else {std::fprintf(stderr,"Usage: AYRenderer_ColorStylizationDemo [--gallery | --motion-gallery] [--animate-palette] [--hidden] [--d3d12] [--output directory] [--look file.rsl] [--write-presets] [--scene auto|gallery|city|character|workshop|observatory|canal]\n");return 2;}
    }
    std::filesystem::create_directories(output);
    if(writePresets) {
        for(int i=0;i<lookCount;++i) {
            std::string error;
            if(!saveColorStylizationLook((output/(std::to_string(i)+".rsl")).string(),
                colorStylizationPreset(static_cast<ColorStylizationPreset>(i)),&error)) {
                std::fprintf(stderr,"%s\n",error.c_str());return 1;
            }
        }
        if(!gallery)return 0;
    }
    ColorStylizationSettings customLook;
    if(!lookPath.empty()) {
        std::string error;if(!loadColorStylizationLook(lookPath,customLook,&error)) {
            std::fprintf(stderr,"%s\n",error.c_str());return 1;
        }
        if(animatePalette && (!customLook.enabled || customLook.colorMapping!=StylizationColorMapping::FixedPalette)) {
            std::fprintf(stderr,"--animate-palette requires an enabled Fixed Palette look\n");return 2;
        }
    }
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
    const auto sphere=demo::uploadGalleryGeometry(renderer,demo::gallerySphere());
    const auto cylinder=demo::uploadGalleryGeometry(renderer,demo::galleryCylinder());
    const auto torus=demo::uploadGalleryGeometry(renderer,demo::galleryTorus());
    const auto roof=demo::uploadGalleryGeometry(renderer,demo::galleryRoof());
    const uint8_t white[4]={255,255,255,255};
    auto whiteTexture=renderer.createTextureFromRgba8(1,1,white,"retro_gallery_white");
    std::vector<TextureHandle> textures{whiteTexture};
    std::vector<MaterialHandle> materials;
    auto material=[&](float r,float g,float b,TextureHandle texture) {
        auto value=renderer.createMaterialFromPhoskia(kPbrPhoskiaSource);
        renderer.setMaterialTexture(value,"baseColorTexture",texture);
        renderer.setMaterialColor(value,"baseColor",r,g,b,1);
        renderer.setMaterialFloat(value,"metallic",0);
        renderer.setMaterialFloat(value,"roughness",.65f);
        renderer.setMaterialFloat(value,"ao",1);
        renderer.setMaterialFloat(value,"opacity",1);
        renderer.setMaterialFloat(value,"normalYSign",1);
        for (const char* name : {"opacitySource","doubleSided","premultipliedAlpha","castSkinned"})
            renderer.setMaterialFloat(value,name,0);
        renderer.setMaterialVec3(value,"emissive",0,0,0);
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
    std::vector<uint8_t> planetPixels(256*128*4);
    for(int y=0;y<128;++y)for(int x=0;x<256;++x) {
        const float u=float(x)/256,v=float(y)/128;
        const float terrain=std::sin(u*19.f+std::sin(v*13.f)*2.f)*std::cos(v*11.f)+std::sin(u*31.f-v*17.f)*.3f;
        const std::array<uint8_t,3> color=std::abs(v-.5f)>.42f?std::array<uint8_t,3>{232,230,207}
            :terrain>.25f?std::array<uint8_t,3>{114,157,87}:terrain>.08f?std::array<uint8_t,3>{208,177,102}
            :std::array<uint8_t,3>{32,102,161};
        for(int c=0;c<3;++c)planetPixels[(y*256+x)*4+c]=color[c];planetPixels[(y*256+x)*4+3]=255;
    }
    const auto planetTexture=renderer.createTextureFromRgba8(256,128,planetPixels.data(),"gallery_planet");textures.push_back(planetTexture);
    const auto planetMaterial=material(1,1,1,planetTexture),brass=material(.82f,.55f,.19f,whiteTexture),water=material(.06f,.34f,.48f,whiteTexture);
    renderer.setMaterialFloat(brass,"metallic",.9f);renderer.setMaterialFloat(brass,"roughness",.18f);
    renderer.setMaterialFloat(water,"metallic",.2f);renderer.setMaterialFloat(water,"roughness",.16f);
    std::array<MaterialHandle,8> surfaces{};
    for(int i=0;i<8;++i) {
        surfaces[i]=material(i<4?.72f:palette[i%6][0],i<4?.78f:palette[i%6][1],i<4?.82f:palette[i%6][2],whiteTexture);
        renderer.setMaterialFloat(surfaces[i],"metallic",i<4?1.f:0.f);
        renderer.setMaterialFloat(surfaces[i],"roughness",.1f+float(i%4)*.27f);
    }
    bool valid=cube.isValid() && sphere.isValid() && cylinder.isValid() && torus.isValid() && roof.isValid()
        && whiteTexture.isValid() && ramp.isValid() && planetTexture.isValid();
    for(auto value:materials)valid=valid && value.isValid();
    if(!valid){renderer.shutdown();DestroyWindow(window);return 1;}
    int preset=animatePalette?int(ColorStylizationPreset::Sunset):1;bool deferred=false,paused=false,customSelected=!lookPath.empty();uint32_t tick=0;int previousGallery=-1;
    const int batchLookCount=animatePalette || !lookPath.empty()?1:motionGallery?5:lookCount,framesPerLook=motionGallery?100:24;
    ColorStylizationSettings paletteBase;
    const auto applyLook=[&](const ColorStylizationSettings& look){renderer.setColorStylizationSettings(look);paletteBase=renderer.colorStylizationSettings();};
    applyLook(lookPath.empty()?colorStylizationPreset(static_cast<ColorStylizationPreset>(preset)):customLook);
    auto previousFrameTime=std::chrono::steady_clock::now();float time=0;
    std::array<bool,256> wasDown{};
    auto pressed=[&](int key) {bool down=(GetAsyncKeyState(key)&0x8000)!=0;bool edge=down&&!wasDown[key];wasDown[key]=down;return edge;};
    std::puts("0..9 / arrows preset | Tab scene | F Forward/Deferred | Space freeze | G color grading | F9 capture | Esc quit");
    int failures=0;
    while(state.running && (!gallery || tick<2*batchLookCount*framesPerLook)) {
        const auto now=std::chrono::steady_clock::now();
        const float elapsed=std::chrono::duration<float>(now-previousFrameTime).count();
        previousFrameTime=now;
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT)state.running=false;
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(!state.running)break;
        const int galleryIndex=int(tick/framesPerLook);
        if(gallery && galleryIndex!=previousGallery) {
            previousGallery=galleryIndex;deferred=galleryIndex>=batchLookCount;preset=galleryIndex%batchLookCount;
            if(motionGallery && !animatePalette && preset>0)preset+=11;
            if(animatePalette && lookPath.empty())preset=int(ColorStylizationPreset::Sunset);
            renderer.configurePipeline(deferred?RenderPipelineDesc::makeDeferred():RenderPipelineDesc::makeDefault());
            renderer.setShadowsEnabled(false);
            applyLook(lookPath.empty()?colorStylizationPreset(static_cast<ColorStylizationPreset>(preset)):customLook);
        }
        if(!gallery && GetForegroundWindow()==window) {
            for(int i=0;i<10;++i)if(pressed('0'+i)) {
                customSelected=false;
                preset=i;applyLook(colorStylizationPreset(static_cast<ColorStylizationPreset>(i)));
            }
            const bool previousLook=pressed(VK_LEFT),nextLook=pressed(VK_RIGHT);
            if(previousLook || nextLook) {
                customSelected=false;
                preset=(preset+(previousLook?lookCount-1:1))%lookCount;applyLook(colorStylizationPreset(static_cast<ColorStylizationPreset>(preset)));
            }
            if(pressed(VK_TAB)){sceneOverride=(sceneOverride+1)%6;renderer.notifyCameraCut();}
            if(pressed('F')){deferred=!deferred;renderer.configurePipeline(deferred?RenderPipelineDesc::makeDeferred():RenderPipelineDesc::makeDefault());}
            if(pressed(VK_SPACE))paused=!paused;
            if(pressed('G')){renderer.setColorGradingPreset(ColorGradingPreset::Warm);renderer.setColorGradingEnabled(!renderer.colorGradingEnabled());}
        }
        if(gallery)time=motionGallery?float(tick%framesPerLook)/30.f:.8f;
        else if(!paused)time+=elapsed;
        if(animatePalette) {
            const float paletteTime=gallery?float(tick%framesPerLook)/30.f:time;
            renderer.setColorStylizationSettings(demo::palettePerformance(paletteBase,paletteTime));
            if(gallery)time=.8f; // Keep geometry fixed in exported palette performances.
        }
        RenderScene scene;
        scene.add(cube,floorMaterial,translate(0,-.2f,1)*scale(12,.25f,9));
        const int exhibit=sceneOverride>=0?sceneOverride:motionGallery?2:preset>=16?4:preset>=12?2:preset==10?2:preset==11?1:preset<6?0:preset==6?1:preset==7?2:3;
        if(exhibit==5)renderer.setMainCameraLookAtPerspective({8,10,-10},{0,.9f,1},{0,1,0},45,float(width)/height,.1f,100);
        else renderer.setMainCameraLookAtPerspective({8,6,-11},{0,1.1f,1},{0,1,0},48,float(width)/height,.1f,100);
        if(exhibit==0) {
        scene.add(cube,rampMaterial,translate(0,2.3f,4)*scale(10,3,.2f));
        for(int i=0;i<6;++i) {
            const float x=(i-2.5f)*1.65f;
            scene.add(cube,colors[i],translate(x,.35f,1)*scale(1.35f,.65f,1.35f));
            scene.add(cube,colors[i],translate(x,1.4f,1)*rotate(FVector3(0,1,0),time*.35f+i*.2f)*scale(.85f,1.1f,.85f));
            for(int j=0;j<4;++j)scene.add(cube,colors[i],translate(x,.15f+j*.2f,-1.7f+j*.28f)*scale(.9f,.15f,.22f));
        }
        } else if(exhibit==1) {
            // Noir city: layered silhouettes, lit windows and a bridge.
            for(int i=0;i<13;++i) {
                float x=(i-6)*.8f,h=1.4f+float((i*7)%9)*.35f;
                scene.add(cube,colors[(i+4)%6],translate(x,h*.5f,3)*scale(.65f,h,1));
                for(int y=0;y<int(h*2);++y)for(int column=0;column<2;++column)
                    scene.add(cube,colors[(i+y)%6],translate(x-.16f+column*.32f,.3f+y*.45f,2.48f)*scale(.12f,.19f,.035f));
            }
            for(int i=0;i<10;++i) {
                scene.add(cube,floorMaterial,translate((i-4.5f)*.85f,1.1f,-1)*scale(.7f,.15f,1));
                scene.add(cube,colors[0],translate((i-4.5f)*.85f,.5f,-1)*scale(.12f,1,.12f));
            }
        } else if(exhibit==2) {
            // A block-built action character and props, with moving limbs.
            auto part=[&](MaterialHandle m,float x,float y,float z,float sx,float sy,float sz,float angle=0.f) {
                if(motionGallery || preset>=12){x+=std::sin(time*2)*1.7f;y+=std::sin(time*4)*.15f;}
                scene.add(cube,m,translate(x,y,z)*rotate(FVector3(0,0,1),angle)*scale(sx,sy,sz));
            };
            part(colors[0],0,2.3f,1,1.5f,1.5f,.8f);
            part(colors[2],0,3.6f,1,.9f,.9f,.8f);
            part(floorMaterial,0,3.65f,.57f,.7f,.22f,.06f);
            part(colors[4],-.6f,.8f,1,.6f,1.5f,.65f,-.15f);
            part(colors[4],.6f,.8f,1,.6f,1.5f,.65f,.15f);
            part(colors[1],-1.25f,2.4f,1,.55f,1.7f,.55f,.55f+std::sin(time*4)*.35f);
            part(colors[1],1.25f,2.4f,1,.55f,1.7f,.55f,-.55f-std::sin(time*4)*.35f);
            for(int i=0;i<6;++i) {
                float a=i*1.0472f;
                scene.add(cube,colors[i],translate(std::cos(a)*3.4f,.5f+float(i%3)*.3f,1+std::sin(a)*2)*rotate(FVector3(0,0,1),a+time*.2f)*scale(.7f,.7f,.7f));
            }
        } else if(exhibit==3) {
            // Technical workshop: gantry, stacked plates and radial machine.
            for(float x:{-3.f,3.f})scene.add(cube,colors[4],translate(x,1.8f,2)*scale(.5f,3.6f,.6f));
            scene.add(cube,colors[4],translate(0,3.5f,2)*scale(6.5f,.45f,.65f));
            for(int i=0;i<12;++i) {
                float a=i*.523599f+time*.2f;
                scene.add(cube,colors[i%6],translate(std::cos(a)*1.6f,1.8f+std::sin(a)*1.6f,1)*rotate(FVector3(0,0,1),a)*scale(.7f,.3f,.5f));
            }
            for(int i=0;i<6;++i)scene.add(cube,colors[(i+2)%6],translate(3.8f,.3f+i*.27f,-1)*rotate(FVector3(0,1,0),i*.14f)*scale(1.5f,.12f,1.5f));
            scene.add(cube,colors[2],translate(-3.8f,.65f,-1)*scale(1.5f,1.3f,1.5f));
        } else if(exhibit==4) {
            // An orrery: curved normals, a generated globe, brass rings and a roughness row.
            scene.add(cylinder,floorMaterial,translate(0,.25f,1)*scale(3.3f,.55f,3.3f));
            scene.add(cylinder,brass,translate(0,.8f,1)*scale(.45f,1.1f,.45f));
            scene.add(sphere,planetMaterial,translate(0,2.3f,1)*rotate(FVector3(0,1,0),time*.25f)*scale(2.3f,2.3f,2.3f));
            scene.add(torus,brass,translate(0,2.3f,1)*rotate(FVector3(0,0,1),.4f)*scale(3.2f,3.2f,3.2f));
            scene.add(torus,colors[4],translate(0,2.3f,1)*rotate(FVector3(1,0,0),1.1f)*scale(3.5f,3.5f,3.5f));
            for(int i=0;i<5;++i) {
                const float a=time*.5f+i*1.256637f;
                scene.add(sphere,colors[i],translate(std::cos(a)*3.1f,2.1f+std::sin(a*2)*.55f,1+std::sin(a)*2.5f)*scale(.55f,.55f,.55f));
            }
            for(int i=0;i<8;++i) {
                const float x=(i-3.5f)*1.1f;
                scene.add(cylinder,brass,translate(x,.18f,-2)*scale(.75f,.35f,.75f));
                scene.add(sphere,surfaces[i],translate(x,.73f,-2)*scale(.8f,.8f,.8f));
            }
        } else {
            // A small canal district: pitched roofs, windows, bridge arches and boats.
            scene.add(cube,water,translate(0,.01f,1)*scale(11,.13f,3));
            for(int bank=0;bank<2;++bank)for(int i=0;i<4;++i) {
                if(bank==0 && (i==1 || i==2))continue;
                const float x=(i-1.5f)*2.4f,z=bank?3.6f:-1.6f,h=bank?1.4f+float((i+bank)%3)*.4f:1.f;
                scene.add(cube,colors[(i+bank*2)%6],translate(x,h*.5f,z)*scale(1.75f,h,1.6f));
                scene.add(roof,colors[(i+bank+1)%6],translate(x,h,z)*scale(2.05f,1.3f,1.95f));
                for(int row=0;row<2;++row)for(float offset:{-.45f,.45f})
                    scene.add(cube,brass,translate(x+offset,.45f+row*.6f,z+(bank?-.815f:.815f))*scale(.3f,.35f,.045f));
                scene.add(cylinder,brass,translate(x-.7f,.45f,bank?2.5f:-.5f)*scale(.1f,.9f,.1f));
            }
            scene.add(cube,floorMaterial,translate(0,1.05f,1)*scale(1.8f,.18f,3.5f));
            for(float x:{-.85f,.85f})
                scene.add(torus,brass,translate(x,.6f,1)*rotate(FVector3(0,0,1),1.5707963f)*scale(3.f,3.f,3.f));
            for(float x:{-.65f,.65f})for(float z:{-.65f,2.65f})
                scene.add(cylinder,brass,translate(x,.5f,z)*scale(.16f,1,.16f));
            for(int i=0;i<5;++i)
                scene.add(cube,floorMaterial,translate(0,.94f-i*.18f,-1.f-i*.28f)*scale(1.4f,.18f,.35f));
            for(int i=0;i<3;++i) {
                const float x=(i-1.f)*3.3f+std::sin(time*.7f+i)*.35f,z=.3f+i*.45f;
                scene.add(cylinder,colors[(i+1)%6],translate(x,.28f,z)*rotate(FVector3(0,0,1),1.5707963f)*scale(.55f,1.8f,.55f));
                scene.add(cube,colors[(i+3)%6],translate(x,.59f,z)*scale(.7f,.35f,.4f));
                scene.add(cylinder,brass,translate(x+.2f,1.03f,z)*scale(.06f,.9f,.06f));
            }
        }
        ClearDesc clear;clear.r=.025f;clear.g=.04f;clear.b=.08f;
        renderer.beginFrame(clear);renderer.render(scene);
        const int localFrame=int(tick%framesPerLook);
        bool capture=gallery ? (motionGallery?localFrame>=4 && localFrame%3==1:localFrame==20)
            : GetForegroundWindow()==window && pressed(VK_F9);
        if(capture) {
            const auto base=output/((deferred?std::string("deferred-"):std::string("forward-"))+std::to_string(animatePalette?0:preset)
                +(motionGallery?"-"+std::to_string(localFrame):""));
            if(!renderer.captureScreenshot(base.string()+".png"))++failures;
            std::printf("Capture %s\n",base.string().c_str());
        }
        renderer.endFrame();
        if(gallery && localFrame>=8) {
            const auto& stats=renderer.getFrameStats();
            uint32_t draws=0;for(const auto& pass:stats.passes)if(pass.name=="ColorStylization")draws+=pass.drawCalls;
            if(!stats.graph.compileSucceeded || draws!=(colorStylizationActive(renderer.colorStylizationSettings())?1u:0u)) {
                ++failures;std::fprintf(stderr,"Gallery graph/draw failure tick=%u draws=%u compiled=%d\n",tick,draws,int(stats.graph.compileSucceeded));
            }
        }
        ++tick;
        std::wstring title=L"Retro Color Gallery | ";title+=deferred?L"Deferred | ":L"Forward | ";
        for(const char* p=animatePalette?"Palette Morph":customSelected?"Custom Look":names[preset];*p;++p)title+=wchar_t(*p);
        title+=L" | ";for(const char* p=sceneNames[exhibit];*p;++p)title+=wchar_t(*p);
        title+=L" | arrows look, Tab scene, F pipeline, Space freeze, F9 save";
        SetWindowTextW(window,title.c_str());
    }
    for(auto value:materials)renderer.destroyMaterial(value);
    for(auto value:textures)renderer.destroyTexture(value);
    for(auto mesh:{cube,sphere,cylinder,torus,roof})renderer.destroyMesh(mesh);
    renderer.shutdown();DestroyWindow(window);
    if(gallery)for(const char* path:{"forward","deferred"})for(int i=0;i<batchLookCount;++i) {
        const int look=motionGallery && i>0?i+11:i;
        if(motionGallery)for(int frame=4;frame<framesPerLook;frame+=3) {
            if(!std::filesystem::exists(output/(std::string(path)+"-"+std::to_string(look)+"-"+std::to_string(frame)+".png")))++failures;
        } else if(!std::filesystem::exists(output/(std::string(path)+"-"+std::to_string(look)+".png")))++failures;
    }
    std::printf("Gallery completed, failures=%d\n",failures);
    return failures?1:0;
}

int wmain(int argc,wchar_t** argv) {
    ayt::render::demo::Utf8Arguments arguments(argc,argv);
    return runGallery(argc,arguments.pointers.data());
}
