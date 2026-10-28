#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include <bgfx/bgfx.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ayt::render;
namespace {
nlohmann::json distribution(std::vector<double> values)
{
    if(values.empty())return {{"samples",0}};
    std::sort(values.begin(),values.end());
    return {{"samples",values.size()},{"median_ms",values[values.size()/2]},
        {"p95_ms",values[std::min(values.size()-1,values.size()*95/100)]}};
}
}

int main(int argc,char** argv)
{
    if((argc!=3 && argc!=4) || (std::string(argv[1])!="d3d11" && std::string(argv[1])!="d3d12")
        || (argc==4 && std::string(argv[3])!="0" && std::string(argv[3])!="2" && std::string(argv[3])!="4")) {
        std::cerr<<"Usage: AYRenderer_PresentationGpuBench d3d11|d3d12 report.json [0|2|4 MSAA]\n";return 2;
    }
    HWND window=CreateWindowExW(0,L"STATIC",L"Presentation cost probe",WS_POPUP,
        0,0,3840,2160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window)return 2;
    struct WindowCleanup{HWND handle;~WindowCleanup(){DestroyWindow(handle);}} cleanup{window};
    Renderer renderer;InitDesc init;init.windowHandle=window;init.width=1920;init.height=1080;
    init.msaa=argc==4?uint32_t(argv[3][0]-'0'):0;init.vsync=false;
    init.backend=std::string(argv[1])=="d3d12"?Backend::Direct3D12:Backend::Direct3D11;
    if(!renderer.initialize(init))return 2;
    bgfx::setDebug(BGFX_DEBUG_PROFILER);
    renderer.setFxaaEnabled(false);renderer.setSmaaEnabled(false);renderer.setTaaEnabled(false);
    renderer.setShadowsEnabled(false);renderer.setPostProcessBloomStrength(0);
    renderer.setPostProcessGamma(1);renderer.setPostProcessExposure(1);
    renderer.setPostProcessTonemapMode(Renderer::TonemapMode::None);
    ColorStylizationSettings look;look.enabled=true;look.pixelationEnabled=false;look.posterizationEnabled=false;
    look.display.enabled=true;look.display.scanlineStrength=.3f;look.display.maskStrength=.2f;
    look.display.chromaticOffset=1;look.display.curvature=.06f;look.display.vignetteStrength=.2f;
    look.display.grainStrength=.05f;look.display.noiseSeed=42;
    renderer.setColorStylizationSettings(look);
    const auto* caps=bgfx::getCaps();
    nlohmann::json report={{"schema_version",1},{"backend",argv[1]},
        {"renderer",bgfx::getRendererName(bgfx::getRendererType())},
        {"vendor_id",caps?caps->vendorId:0},{"device_id",caps?caps->deviceId:0},
        {"msaa",init.msaa},
#ifdef NDEBUG
        {"configuration","Release"},
#else
        {"configuration","Debug"},
#endif
        {"scope","Empty main Forward scene plus final color/display; isolate presentation cost, not a game benchmark"},
        {"notes","24 warm-up and 48 sampled frames per ABBA block; GPU intervals use distinct completed frames and may lag; pooled memory can remain cached"},
        {"blocks",nlohmann::json::array()}};
    RenderScene scene;bool valid=true;
    for(const auto extent:std::array<std::array<uint32_t,2>,2>{{{1920,1080},{3840,2160}}}) {
        renderer.resize(extent[0],extent[1]);
        renderer.setViewportRect(0,0,static_cast<uint16_t>(extent[0]),static_cast<uint16_t>(extent[1]));
        for(size_t block=0;block<4;++block) {
            const bool integrated=block==1 || block==2;
            auto desc=RenderPipelineDesc::makeDefault();desc.integratePresentation=integrated;
            renderer.configurePipeline(desc);renderer.setShadowsEnabled(false);
            renderer.setFxaaEnabled(false);renderer.setSmaaEnabled(false);renderer.setTaaEnabled(false);
            std::vector<double> wall,cpu,gpu,presentationGpu;
            uint32_t previousGpuFrame=UINT32_MAX,draws=0,backendDraws=0,presents=0;
            uint16_t targets=0;
            for(int frame=0;frame<72;++frame) {
                const auto begin=std::chrono::steady_clock::now();
                ClearDesc clear;clear.r=.2f;clear.g=.4f;clear.b=.6f;
                renderer.beginFrame(clear);renderer.render(scene);renderer.endFrame();
                if(frame<24)continue;
                const auto& stats=renderer.getFrameStats();
                valid &= stats.graph.compileSucceeded;
                uint32_t presentDraws=0;double stageGpu=0;
                for(const auto& pass:stats.passes) {
                    if(pass.name=="Present"){presentDraws+=pass.drawCalls;stageGpu+=pass.gpuTimeMs;}
                    if(pass.name=="AnalogDisplay")stageGpu+=pass.gpuTimeMs;
                }
                valid &= presentDraws==(integrated?0u:1u);
                wall.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
                cpu.push_back(stats.renderCpuTimeMs);
                if(stats.gpuFrameNumber!=previousGpuFrame) {
                    previousGpuFrame=stats.gpuFrameNumber;
                    if(stats.gpuFrameTimeMs>0)gpu.push_back(stats.gpuFrameTimeMs);
                    if(stageGpu>0)presentationGpu.push_back(stageGpu);
                }
                draws=stats.drawCalls;backendDraws=stats.backendDrawCalls;presents=presentDraws;
                targets=stats.graph.transientTargets;
            }
            report["blocks"].push_back({{"width",extent[0]},{"height",extent[1]},
                {"block",block},{"integrated",integrated},{"draws",draws},{"backend_draws",backendDraws},
                {"present_draws",presents},{"planned_targets",targets},
                {"wall",distribution(std::move(wall))},{"render_cpu",distribution(std::move(cpu))},
                {"gpu_frame",distribution(std::move(gpu))},{"display_and_present_gpu",distribution(std::move(presentationGpu))}});
        }
    }
    report["valid"]=valid;
    renderer.shutdown();
    std::ofstream output(argv[2],std::ios::binary);output<<report.dump(2)<<'\n';output.close();
    if(!output)return 2;
    std::cout<<report.dump(2)<<'\n';return valid?0:1;
}
