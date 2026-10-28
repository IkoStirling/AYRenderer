#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "detail/PresentationPass.h"
#include "detail/PresentationStage.h"
#include "detail/PresentPass.h"
#include "detail/RenderPipeline.h"
#include "detail/RenderPipelineLifecycle.h"
#include "detail/RenderViewCatalog.h"
#include "detail/FullscreenPassGeometry.h"
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
using namespace ayt::render::detail;
namespace {
constexpr const char* kCopySource=R"(
material ChainCostCopy {
    texture2d inputColor
    vertex {
        in pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        return sample(inputColor, vec2(vUv.x, 1.0 - vUv.y))
    }
}
)";
constexpr const char* kCopyKey="bench_presentation_chain_copy_v1";

// Intentionally a one-input, one-draw transport workload. Reusing a program
// isolates target/draw/bandwidth growth; it does not model distinct algorithms,
// expensive shader ALU, temporal preparation or additional PSO switches.
class CopyStage final : public PresentationPass {
public:
    explicit CopyStage(const PresentationStageInfo& info):PresentationPass(info) {}
    bool active() const noexcept override {return true;}
    uint32_t execute(PassExecContext& context) override {
        PresentationStageTargets targets;
        if(!resolveTargets(context,targets) || !_geometry.ensure(context.adapter))return 0;
        if(!_program.isValid())_program=context.pool.acquire(kCopySource,kCopyKey);
        if(!_program.isValid())return 0;
        const auto binding=_program.getTextureBinding("inputColor");
        if(binding==ayt::shader::InvalidBinding)return 0;
        configureFullscreenPassView(context.adapter,presentationInfo().view,targets.target,
            targets.x,targets.y,context.viewportWidth,context.viewportHeight);
        _geometry.bind(context.adapter);
        _program.setTexture(_program.getTextureStage(binding),binding,toShaderTexture(targets.input));
        context.adapter.setStateDepthTestAlways();
        _program.submit({presentationInfo().view,0});publishOutput(context);return 1;
    }
    void destroyResources(BGFXAdapter& adapter) override {_program.reset();_geometry.destroy(adapter);}
private:
    FullscreenPassGeometry _geometry;
    ayt::shader::ShaderResource _program;
};

nlohmann::json distribution(std::vector<double> values)
{
    if(values.empty())return {{"samples",0}};
    std::sort(values.begin(),values.end());
    return {{"samples",values.size()},{"median_ms",values[values.size()/2]},
        {"p95_ms",values[std::min(values.size()-1,values.size()*95/100)]}};
}
double milliseconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double,std::milli>(duration).count();
}
double ticks(int64_t begin,int64_t end,int64_t frequency)
{
    return frequency>0 && end>begin?double(end-begin)*1000.0/double(frequency):0;
}
}

int main(int argc,char** argv) try
{
    if(argc!=3 || (std::string(argv[1])!="d3d11" && std::string(argv[1])!="d3d12")) {
        std::cerr<<"Usage: AYRenderer_PresentationScaleBench d3d11|d3d12 report.json\n";return 2;
    }
    HWND window=CreateWindowExW(0,L"STATIC",L"Presentation scale probe",WS_POPUP,
        0,0,3840,2160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window)return 2;
    struct WindowCleanup{HWND h;~WindowCleanup(){DestroyWindow(h);}} windowCleanup{window};
    BGFXAdapter adapter;
    struct AdapterCleanup{BGFXAdapter& a;~AdapterCleanup(){a.shutdown();}} adapterCleanup{adapter};
    BGFXInitParams init;init.nativeWindowHandle=window;init.width=1920;init.height=1080;
    init.msaa=0;init.vsync=false;
    init.backend=std::string(argv[1])=="d3d12"?Backend::Direct3D12:Backend::Direct3D11;
    if(!adapter.initialize(init))return 2;
    bgfx::setDebug(BGFX_DEBUG_PROFILER);
    ayt::shader::ShaderResourcePool shaders;
    struct ShaderCleanup{ayt::shader::ShaderResourcePool& p;~ShaderCleanup(){p.shutdown();}} shaderCleanup{shaders};
    shaders.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    shaders.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT,AY_SHADER_BGFX_SRC_HINT});shaders.resolvePlatformFromRenderer();
    auto program=shaders.acquire(kCopySource,kCopyKey);
    if(!program.isValid()) {
        for(const auto& error:shaders.lastCompileErrors())std::cerr<<error<<'\n';return 2;
    }
    program.reset();
    constexpr size_t maximum=20;
    std::array<std::string,maximum> names;
    std::array<PresentationStageInfo,maximum> infos;
    std::array<CopyStage*,maximum> stages{};
    RenderPipeline pipeline;
    struct PassCleanup{RenderPipeline& p;BGFXAdapter& a;~PassCleanup(){destroyPipelineGpuResources(p,a);}} passCleanup{pipeline,adapter};
    size_t resource=0;uint16_t view=0;
    for(size_t index=0;index<maximum;++index) {
        while(resource<static_cast<size_t>(FgResourceId::Count)
            && (resource==static_cast<size_t>(FgResourceId::FinalLdrColor)
                || contractResourceFor(static_cast<FgResourceId>(resource))==RenderPassResourceId::Count))++resource;
        while(view<kRenderViewCapacity && (!canReserveMainRenderView(view) || isMainRenderViewReserved(view)))++view;
        if(resource>=static_cast<size_t>(FgResourceId::Count) || view>=kRenderViewCapacity)return 2;
        names[index]="CopyStage"+std::to_string(index);const auto output=static_cast<FgResourceId>(resource++);
        infos[index]={static_cast<RenderPassSlot>(200+index),names[index],output,
            {contractResourceFor(output),RenderPassResourceLifetime::Transient,RenderPassResourceFormat::RGBA8},
            static_cast<uint8_t>(view++),static_cast<uint16_t>(10+index),PresentationStageInputs::Color,
            false,false,names[index],names[index],true};
        auto pass=std::make_unique<CopyStage>(infos[index]);stages[index]=pass.get();pipeline.addPass(std::move(pass));
    }
    pipeline.addPass(RenderPassSlot::Present,std::make_unique<PresentPass>());
    std::array<bgfx::ViewId,kRenderViewCapacity> remap{};
    std::array<bool,kRenderViewCapacity> used{};size_t rank=0;
    for(const auto& info:infos){remap[rank++]=info.view;used[info.view]=true;}
    remap[rank++]=PresentPass::kPresentViewId;used[PresentPass::kPresentViewId]=true;
    for(size_t id=0;id<used.size();++id)if(!used[id])remap[rank++]=static_cast<bgfx::ViewId>(id);
    adapter.setViewOrder(0,static_cast<uint16_t>(remap.size()),remap.data());
    const auto* caps=bgfx::getCaps();
    nlohmann::json report={{"schema_version",1},{"backend",argv[1]},
        {"renderer",bgfx::getRendererName(bgfx::getRendererType())},
        {"vendor_id",caps?caps->vendorId:0},{"device_id",caps?caps->deviceId:0},
#ifdef NDEBUG
        {"configuration","Release"},
#else
        {"configuration","Debug"},
#endif
        {"scope","20 mounted test-only one-input copy stages; vary active count; no scene or effect algorithm"},
        {"notes","MSAA0; profiler enabled; 24 warm-up and 48 sampled frames in ABBA blocks; GPU samples deduplicated by completed frame; same program reused, not a PSO-switch benchmark; pool bytes are logical estimates, not driver allocation"},
        {"blocks",nlohmann::json::array()}};
    bool valid=true;
    FrameContext frame;RenderScene scene;
    std::unordered_map<uint64_t,GpuMesh> meshes;
    std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    std::vector<PresentationStageFrame> contributions;
    for(const auto extent:std::array<std::array<uint16_t,2>,2>{{{1920,1080},{3840,2160}}}) {
        const auto width=extent[0],height=extent[1];adapter.resetResolution(width,height,false);
        RenderTargetPool targets(adapter);FrameGraph graph(adapter,targets);
        struct Input {
            BGFXAdapter& adapter;
            bgfx::FrameBufferHandle framebuffer=BGFX_INVALID_HANDLE;
            bgfx::TextureHandle texture=BGFX_INVALID_HANDLE;
            ~Input(){adapter.destroy(framebuffer);adapter.destroy(texture);}
        } input{adapter};
        std::vector<uint8_t> texels(size_t(width)*height*4);uint32_t random=0x3f19a726;
        for(size_t pixel=0;pixel<texels.size();pixel+=4) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            texels[pixel]=uint8_t(random);texels[pixel+1]=uint8_t(random>>8);
            texels[pixel+2]=uint8_t(random>>16);texels[pixel+3]=255;
        }
        input.texture=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_RT|BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,
            bgfx::copy(texels.data(),static_cast<uint32_t>(texels.size())));
        if(!bgfx::isValid(input.texture))return 2;
        input.framebuffer=bgfx::createFrameBuffer(1,&input.texture,false);
        if(!bgfx::isValid(input.framebuffer))return 2;
        PassExecContext execution{adapter,shaders,scene,meshes,textures,materials,0,0,width,height,frame};execution.frameGraph=&graph;
        PresentationFrameContext preparation{adapter,shaders,frame,0,0,width,height,true,input.framebuffer};
        for(const size_t count:{0u,1u,5u,10u,20u}) {
            for(size_t index=0;index<stages.size();++index)stages[index]->setEnabled(index<count);
            for(size_t block=0;block<4;++block) {
                const bool integrated=block==1 || block==2;
                std::vector<double> plan,submit,wall,gpu,chainGpu;
                uint32_t previousGpuFrame=UINT32_MAX,draws=0,backendDraws=0;
                for(int tick=0;tick<72;++tick) {
                    const auto begin=std::chrono::steady_clock::now();targets.beginFrame();
                    preparePresentationStages(pipeline,preparation,contributions);
                    graph.beginFrame(width,height);graph.importExternal(FgResourceId::FinalLdrColor,input.framebuffer);
                    auto source=FgResourceId::FinalLdrColor;
                    const bool declared=declarePresentationStages(graph,contributions,source,nullptr,integrated);
                    graph.addPass({"Present",{source},{},true,true,static_cast<int16_t>(RenderPassSlot::Present)});
                    graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
                    const bool compiled=graph.compile();valid &= declared && compiled;
                    graph.markProduced(FgResourceId::FinalLdrColor);
                    const auto planned=std::chrono::steady_clock::now();
                    draws=pipeline.executeAll(execution);finishPresentationStages(pipeline,nullptr);
                    const auto submitted=std::chrono::steady_clock::now();
                    valid &= draws==count+(integrated && count?0:1);
                    adapter.endFrame();
                    if(tick<24)continue;
                    plan.push_back(milliseconds(planned-begin));submit.push_back(milliseconds(submitted-planned));
                    wall.push_back(milliseconds(std::chrono::steady_clock::now()-begin));
                    const auto* stats=bgfx::getStats();if(!stats)continue;backendDraws=stats->numDraw;
                    if(stats->gpuFrameNum==previousGpuFrame)continue;previousGpuFrame=stats->gpuFrameNum;
                    const auto total=ticks(stats->gpuTimeBegin,stats->gpuTimeEnd,stats->gpuTimerFreq);
                    if(total>0)gpu.push_back(total);
                    double sum=0;
                    for(uint16_t index=0;index<stats->numViews;++index) {
                        const auto& sample=stats->viewStats[index];bool included=sample.view==PresentPass::kPresentViewId;
                        for(size_t stage=0;stage<count && !included;++stage)included=sample.view==infos[stage].view;
                        if(included)sum+=ticks(sample.gpuTimeBegin,sample.gpuTimeEnd,stats->gpuTimerFreq);
                    }
                    if(sum>0)chainGpu.push_back(sum);
                }
                const auto memory=targets.stats();
                report["blocks"].push_back({{"width",width},{"height",height},{"active_stages",count},
                    {"mounted_stages",maximum},{"block",block},{"integrated",integrated},{"draws",draws},
                    {"backend_draws",backendDraws},{"planned_targets",graph.stats().physicalTargets},
                    {"planned_color_bytes",uint64_t(graph.stats().physicalTargets)*width*height*4},
                    {"pool_allocated_bytes",memory.allocatedBytes},{"pool_live_leases",memory.liveLeases},
                    {"pool_idle_targets",memory.idleTargets},{"pool_soft_budget_bytes",memory.budgetBytes},
                    {"plan_cpu",distribution(std::move(plan))},{"submit_cpu",distribution(std::move(submit))},
                    {"wall",distribution(std::move(wall))},{"gpu_frame",distribution(std::move(gpu))},
                    {"chain_gpu",distribution(std::move(chainGpu))}});
            }
        }
        graph.shutdown();targets.shutdown();
    }
    report["valid"]=valid;
    std::ofstream output(argv[2],std::ios::binary);output<<report.dump(2)<<'\n';output.close();
    if(!output)return 2;
    std::cout<<report.dump(2)<<'\n';return valid?0:1;
}
catch(const std::exception& error)
{
    std::cerr<<"Presentation scale probe failed: "<<error.what()<<'\n';return 2;
}
