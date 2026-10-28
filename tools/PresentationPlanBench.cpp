// Short CPU-only scaling probe. No device, shader compilation or wall-clock
// soak is involved. The executable's allocator counter is confined to this tool.
#include "detail/PresentationStagePlan.h"
#include "detail/BGFXAdapter.h"
#include "detail/RenderViewCatalog.h"
#include "detail/RenderViewOrder.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<bool> countAllocations=false;
std::atomic<uint64_t> allocations=0;
void* allocate(size_t size)
{
    if(countAllocations.load(std::memory_order_relaxed))
        allocations.fetch_add(1,std::memory_order_relaxed);
    if(void* result=std::malloc(size?size:1))return result;
    throw std::bad_alloc();
}
}
void* operator new(size_t size) {return allocate(size);}
void* operator new[](size_t size) {return allocate(size);}
void operator delete(void* pointer) noexcept {std::free(pointer);}
void operator delete[](void* pointer) noexcept {std::free(pointer);}
void operator delete(void* pointer,size_t) noexcept {std::free(pointer);}
void operator delete[](void* pointer,size_t) noexcept {std::free(pointer);}

using namespace ayt::render;
using namespace ayt::render::detail;

int main(int argc,char** argv)
{
    if(argc>2) {std::cerr<<"Usage: AYRenderer_PresentationPlanBench [report.json]\n";return 2;}
    constexpr size_t maximum=20;
    std::array<std::string,maximum> names;
    std::array<PresentationStageInfo,maximum> descriptions;
    std::array<PresentationStageFrame,maximum> stages;
    size_t output=0;
    uint16_t view=0;
    for(size_t i=0;i<maximum;++i) {
        while(output<kFrameGraphResourceNumbers.size()
            && (output==static_cast<size_t>(FgResourceId::FinalLdrColor)
                || contractResourceFor(static_cast<FgResourceId>(output))==RenderPassResourceId::Count))++output;
        if(output>=kFrameGraphResourceNumbers.size()) {std::cerr<<"Not enough shared probe resources\n";return 2;}
        while(view<kRenderViewCapacity && (!canReserveMainRenderView(view) || isMainRenderViewReserved(view)))++view;
        if(view>=kRenderViewCapacity) {std::cerr<<"Not enough free probe Views\n";return 2;}
        names[i]="Probe"+std::to_string(i);
        const auto resource=static_cast<FgResourceId>(output++);
        descriptions[i]={static_cast<RenderPassSlot>(160+i),names[i],resource,
            {contractResourceFor(resource),RenderPassResourceLifetime::Transient,RenderPassResourceFormat::RGBA8},
            static_cast<uint8_t>(view++),static_cast<uint16_t>(10+i),PresentationStageInputs::Color,
            false,false,names[i],names[i]};
        stages[i].info=&descriptions[i];stages[i].enabled=true;
    }
    BGFXAdapter adapter;FrameGraph graph(adapter);
    nlohmann::json report={{"schema_version",1},{"scope","CPU declaration, validation and graph compile; no GPU"},
        {"timing_unit","microseconds per plan; median/p95 of batch averages"},
#ifdef NDEBUG
        {"configuration","Release"},
#else
        {"configuration","Debug"},
#endif
        {"samples",nlohmann::json::array()}};
    bool valid=true;
    for(bool sharedInput:{false,true})for(const size_t count:{0u,1u,5u,10u,20u}) {
        for(auto& stage:stages) {
            stage.extraReads.clear();
            if(sharedInput)stage.extraReads.push_back(FgResourceId::FeedbackHistory);
        }
        const auto build=[&] {
            graph.beginFrame(1920,1080);
            graph.importExternal(FgResourceId::FinalLdrColor,{70});
            if(sharedInput)graph.importExternal(FgResourceId::FeedbackHistory,{71});
            auto source=FgResourceId::FinalLdrColor;
            if(!declarePresentationStages(graph,{stages.data(),count},source))return false;
            graph.addPass({"Present",{source},{},true,true});
            graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
            return graph.compile();
        };
        for(int warmup=0;warmup<32;++warmup)valid &= build();
        std::array<double,12> timings;
        for(auto& timing:timings) {
            constexpr int batch=250;
            const auto start=std::chrono::steady_clock::now();
            for(int frame=0;frame<batch;++frame)valid &= build();
            timing=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/batch;
        }
        std::sort(timings.begin(),timings.end());
        allocations.store(0,std::memory_order_relaxed);countAllocations.store(true,std::memory_order_relaxed);
        constexpr int countedFrames=32;
        for(int frame=0;frame<countedFrames;++frame)valid &= build();
        countAllocations.store(false,std::memory_order_relaxed);
        const auto allocationCount=allocations.load(std::memory_order_relaxed);
        report["samples"].push_back({{"stages",count},{"shared_external_input",sharedInput},
            {"median_us",timings[timings.size()/2]},
            {"p95_us",timings.back()},{"allocations_per_plan",double(allocationCount)/countedFrames},
            {"logical_passes",graph.stats().livePasses},{"owned_targets",graph.stats().physicalTargets}});
    }
    report["valid"]=valid;
    report["main_view_order"]=nlohmann::json::array();
    for(const auto& view:kOrderedRenderViews)report["main_view_order"].push_back(view.view);
    report["view_remap"]=kRenderViewRemap;
    if(argc==2) {
        std::ofstream file(argv[1],std::ios::binary);file<<report.dump(2)<<'\n';file.close();
        if(!file) {std::cerr<<"Cannot write report\n";return 2;}
    }
    std::cout<<report.dump(2)<<'\n';
    return valid?0:1;
}
