#include "AYTest.h"
#include "detail/PresentationPass.h"
#include "detail/PostProcessGraphPlan.h"
#include "detail/RenderPipeline.h"
#include "detail/RenderPipelineFactory.h"
#include "detail/RenderViewNamespaces.h"
#include "detail/BuiltinShaderCatalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <stdexcept>

using namespace ayt::render;
using namespace ayt::render::detail;

namespace {
uint8_t unusedProbeView(size_t ordinal)
{
    for(uint16_t view=0;view<kRenderViewCapacity;++view)
        if(canReserveMainRenderView(view) && !isMainRenderViewReserved(view)) {
            if(ordinal==0)return static_cast<uint8_t>(view);--ordinal;
        }
    return UINT8_MAX;
}

PresentationStageInfo probeInfo(uint8_t slot, const char* name, FgResourceId output,
    RenderPassResourceId contractOutput, uint16_t order, bool history=false)
{
    return {static_cast<RenderPassSlot>(slot),name,output,
        {contractOutput,history?RenderPassResourceLifetime::PersistentHistory:
            RenderPassResourceLifetime::Transient,RenderPassResourceFormat::RGBA8},
        unusedProbeView(slot-200),order,history?PresentationStageInputs::History:PresentationStageInputs::Color,
        false,history,name};
}

class ExtensionProbe final : public PresentationPass {
public:
    explicit ExtensionProbe(const PresentationStageInfo& info) : PresentationPass(info) {}
    std::string_view name() const override {return presentationInfo().name;}
    bool active() const noexcept override {return selected;}
    uint32_t execute(PassExecContext& context) override {
        ++executions;
        observedSource=context.frameGraph->resolveSemantic(FgSemantic::PresentSource).idx;
        if(failSubmit)return 0;
        context.frameGraph->markProduced(presentationInfo().output);
        context.frameGraph->setResolvedSemantic(FgSemantic::PresentSource,presentationInfo().output);
        return 1;
    }
    void destroyResources(BGFXAdapter&) override {++cleanups;}
    void applyPresentationLook(const ColorStylizationSettings&,BGFXAdapter&,
        RenderResourceBlackboard*) override {++configurations;}
    void invalidatePresentationHistory(ResourceInvalidationReason reason,
        RenderResourceBlackboard*) override {++invalidations;lastReason=reason;}
    void finishPresentationFrame(RenderResourceBlackboard*) override {++finishes;}
    bool selected=true, failPrepare=false, failSubmit=false;
    int throwPrepare=0;
    bool partialPrepareFailure=false;
    const PresentationStageInfo* replaceInfo=nullptr;
    int preparations=0,executions=0,finishes=0,configurations=0,invalidations=0,cleanups=0;
    uint16_t observedSource=UINT16_MAX;
    ResourceInvalidationReason lastReason=ResourceInvalidationReason::None;
protected:
    void preparePresentationResources(PresentationFrameContext&,PresentationStageFrame& stage) override {
        ++preparations;
        if(replaceInfo)stage.info=replaceInfo;
        if(partialPrepareFailure) {
            stage.imports.push_back({static_cast<FgResourceId>(255),{99}});
            stage.extraReads.push_back(static_cast<FgResourceId>(255));stage.historyTarget={99};
            stage.enabled=false;return;
        }
        if(throwPrepare) {
            stage.imports.push_back({static_cast<FgResourceId>(255),{99}});
            stage.extraReads.push_back(static_cast<FgResourceId>(255));stage.historyTarget={99};
            if(throwPrepare==1)throw std::runtime_error("probe preparation failure");
            throw 77;
        }
        if(failPrepare)stage.enabled=false;
    }
};

PostProcessGraphPlanInput inputForAa(int aa=0)
{
    PostProcessGraphPlanInput input;
    input.width=319;input.height=181;input.sceneColor={70};input.finalLdr=true;
    input.fxaa=aa==1;input.smaa=aa==2;input.taa=aa==3;input.motionVector=aa==3;
    input.taaWriteTarget={71};input.taaReadHistory={72};input.gbuffer={73};input.motionVectors={74};
    return input;
}
}

TEST_SUITE(PresentationStages)

TEST_CASE(catalog_drives_factories_contracts_views_and_shader_jobs)
{
    CHECK(validPresentationStageCatalog(kPresentationStages));
    const auto jobs=builtinShaderJobs();
    for(const auto& info:kPresentationStages) {
        auto pass=makeRenderPass(info.slot);
        auto* stage=dynamic_cast<PresentationPass*>(pass.get());
        CHECK(stage!=nullptr);if(!stage)continue;
        CHECK(&stage->presentationInfo()==&info);
        CHECK(stage->name()==info.name);
        const auto* contract=renderPassContract(info.slot);
        CHECK(contract!=nullptr);if(!contract)continue;
        CHECK(contract->writes.size()==1);
        CHECK(contract->writes.front().resource==info.write.resource);
        CHECK(contract->writes.front().lifetime==info.write.lifetime);
        CHECK(contract->sideEffect==info.history);
        CHECK(std::count(kRenderViewOrder.begin(),kRenderViewOrder.end(),info.view)==1);
        CHECK(presentationStageNameForView(info.view)==info.name);
        CHECK(nextUiViewRange(info.view,1)!=info.view);
        CHECK(std::count_if(jobs.begin(),jobs.end(),[&](const auto& job){
            return job.name==info.shaderJob && job.variant=="default";
        })==1);
    }
    auto bad=kPresentationStages;
    bad[1].view=bad[0].view;CHECK_FALSE(validPresentationStageCatalog(bad));
    bad=kPresentationStages;bad[1].order=bad[0].order;CHECK_FALSE(validPresentationStageCatalog(bad));
    bad=kPresentationStages;bad[1].write.lifetime=RenderPassResourceLifetime::External;
    CHECK_FALSE(validPresentationStageCatalog(bad));
    bad=kPresentationStages;bad[1].shaderJob=bad[0].shaderJob;
    CHECK_FALSE(validPresentationStageCatalog(bad));
    for(uint8_t view:{uint8_t(kCameraFirstView),uint8_t(kCameraLastView),uint8_t(kCameraReadbackView),uint8_t(kDefaultUiView)}) {
        bad=kPresentationStages;bad[1].view=view;CHECK_FALSE(validPresentationStageCatalog(bad));
    }
}

TEST_CASE(all_effect_masks_connect_to_the_selected_aa_without_disabled_targets)
{
    BGFXAdapter adapter;FrameGraph graph(adapter);
    for(int aa=0;aa<4;++aa) {
        auto input=inputForAa(aa);
        const auto baseline=buildPostProcessGraphPlan(graph,input);
        CHECK(baseline.compileSucceeded);
        const auto basePasses=graph.stats().livePasses;
        const auto baseTargets=graph.stats().physicalTargets;
        for(unsigned mask=0;mask<(1u<<kPresentationStages.size());++mask) {
            std::vector<PresentationStageFrame> stages;
            auto expected=baseline.antialiasingSource;
            unsigned transient=0;
            for(size_t i=0;i<kPresentationStages.size();++i) {
                const auto& info=kPresentationStages[i];
                PresentationStageFrame stage;stage.info=&info;stage.enabled=mask&(1u<<i);
                if(info.history) {
                    stage.historyTarget={76};stage.imports.push_back({FgResourceId::FeedbackHistory,{77}});
                    stage.extraReads.push_back(FgResourceId::FeedbackHistory);
                }
                if(stage.enabled){expected=info.output;if(!info.history)++transient;}
                stages.push_back(std::move(stage));
            }
            input.presentationStages=stages;
            const auto result=buildPostProcessGraphPlan(graph,input);
            CHECK(result.compileSucceeded);CHECK(graph.compileErrors().empty());
            CHECK(result.presentationSource==expected);
            CHECK(graph.stats().livePasses==basePasses+std::popcount(mask));
            CHECK(graph.stats().physicalTargets==baseTargets+transient);
            for(size_t i=0;i<kPresentationStages.size();++i) {
                CHECK(graph.shouldExecute(kPresentationStages[i].slot)==bool(mask&(1u<<i)));
                CHECK_FALSE(graph.producedThisFrame(kPresentationStages[i].output));
            }
            CHECK_FALSE(graph.semanticProducedThisFrame(FgSemantic::PresentSource));
            input.presentationStages={};
        }
    }
}

TEST_CASE(unregistered_color_stage_connects_without_downstream_effect_adaptation)
{
    // An isolated fixture reuses an otherwise absent resource identity. A real
    // built-in adds its own append-only IDs and one catalog row, not upstream
    // alternatives to every downstream effect.
    const auto extension=probeInfo(200,"ExtensionColor",FgResourceId::HazeColor,
        RenderPassResourceId::HazeColor,25);
    BGFXAdapter adapter;FrameGraph graph(adapter);
    std::array<PresentationStageFrame,3> stages;
    stages[0].info=presentationStageInfo(RenderPassSlot::ColorGrading);stages[0].enabled=true;
    stages[1].info=&extension;stages[1].enabled=true;
    stages[2].info=presentationStageInfo(RenderPassSlot::ColorStylization);stages[2].enabled=true;
    auto input=inputForAa();input.presentationStages=stages;
    const auto result=buildPostProcessGraphPlan(graph,input);
    CHECK(result.compileSucceeded);
    CHECK(graph.shouldExecute(extension.slot));
    CHECK(graph.shouldExecute(RenderPassSlot::ColorStylization));
    CHECK(graph.stats().livePasses==5);
    CHECK(result.presentationSource==FgResourceId::StylizedColor);
    stages[1].enabled=false;
    const auto bypass=buildPostProcessGraphPlan(graph,input);
    CHECK(bypass.compileSucceeded);CHECK_FALSE(graph.shouldExecute(extension.slot));
    CHECK(graph.stats().livePasses==4);
    CHECK(bypass.presentationSource==result.presentationSource);
}

TEST_CASE(additional_resources_are_declared_by_the_stage_and_missing_inputs_fail_closed)
{
    const auto extension=probeInfo(200,"ExtensionGeometry",FgResourceId::OutlinedColor,
        RenderPassResourceId::OutlinedColor,25);
    BGFXAdapter adapter;FrameGraph graph(adapter);
    PresentationStageFrame stage;stage.info=&extension;stage.enabled=true;
    stage.extraReads={FgResourceId::GBufferNormal};
    auto input=inputForAa();input.presentationStages={&stage,1};
    const auto missing=buildPostProcessGraphPlan(graph,input);
    CHECK_FALSE(missing.compileSucceeded);CHECK_FALSE(graph.shouldExecute(extension.slot));
    CHECK(!graph.compileErrors().empty());
    stage.imports.push_back({FgResourceId::GBufferNormal,{79}});
    const auto ready=buildPostProcessGraphPlan(graph,input);
    CHECK(ready.compileSucceeded);CHECK(graph.shouldExecute(extension.slot));
    CHECK(graph.resolve(FgResourceId::GBufferNormal).idx==79);
    input.finalLdr=false;
    const auto disabled=buildPostProcessGraphPlan(graph,input);
    CHECK(disabled.compileSucceeded);CHECK_FALSE(graph.shouldExecute(extension.slot));
    CHECK(graph.stats().physicalTargets==0);
}

TEST_CASE(common_hooks_follow_mounted_instances_including_disabled_and_failed_preparation)
{
    const auto first=probeInfo(200,"First",FgResourceId::ColorGradedColor,
        RenderPassResourceId::ColorGradedColor,10);
    const auto second=probeInfo(201,"Second",FgResourceId::StylizedColor,
        RenderPassResourceId::StylizedColor,20);
    RenderPipeline pipeline;
    auto a=std::make_unique<ExtensionProbe>(first);auto* pa=a.get();
    auto b=std::make_unique<ExtensionProbe>(second);auto* pb=b.get();
    pipeline.addPass(first.slot,std::move(a));pipeline.addPass(second.slot,std::move(b));
    BGFXAdapter adapter;ayt::shader::ShaderResourcePool pool;FrameContext frame;
    PresentationFrameContext context{adapter,pool,frame,0,0,16,16,true};
    pb->failPrepare=true;
    const auto stages=preparePresentationStages(pipeline,context);
    CHECK(stages.size()==2);CHECK(stages[0].enabled);CHECK_FALSE(stages[1].enabled);
    CHECK(pa->preparations==1);CHECK(pb->preparations==1);
    pb->setEnabled(false);
    const auto disabled=preparePresentationStages(pipeline,context);
    CHECK_FALSE(disabled[1].enabled);CHECK(pb->preparations==1);
    configurePresentationLook(pipeline,{},adapter,nullptr);
    finishPresentationStages(pipeline,nullptr);
    invalidatePresentationHistories(pipeline,ResourceInvalidationReason::CameraCut,nullptr);
    destroyPresentationResources(pipeline,adapter);
    for(auto* pass:{pa,pb}) {
        CHECK(pass->configurations==1);CHECK(pass->finishes==1);
        CHECK(pass->invalidations==1);CHECK(pass->cleanups==1);
        CHECK(pass->lastReason==ResourceInvalidationReason::CameraCut);
    }
    context.colorChainEnabled=false;
    const auto unavailable=preparePresentationStages(pipeline,context);
    CHECK_FALSE(unavailable[0].enabled);CHECK(pa->invalidations==2);
    CHECK(pa->lastReason==ResourceInvalidationReason::ProducerFailure);
    CHECK(pb->invalidations==1);
}

TEST_CASE(failed_stage_keeps_current_source_for_the_following_stage)
{
    std::array<PresentationStageInfo,3> infos{
        probeInfo(200,"First",FgResourceId::ColorGradedColor,RenderPassResourceId::ColorGradedColor,10,true),
        probeInfo(201,"Failing",FgResourceId::StylizedColor,RenderPassResourceId::StylizedColor,20,true),
        probeInfo(202,"Last",FgResourceId::DisplayColor,RenderPassResourceId::DisplayColor,30,true)};
    BGFXAdapter adapter;FrameGraph graph(adapter);RenderPipeline pipeline;
    graph.beginFrame(16,16);graph.importExternal(FgResourceId::FinalLdrColor,{80});
    std::array<PresentationStageFrame,3> stages;
    std::array<ExtensionProbe*,3> probes{};
    for(size_t i=0;i<infos.size();++i) {
        stages[i].info=&infos[i];stages[i].enabled=true;stages[i].historyTarget={uint16_t(81+i)};
        auto pass=std::make_unique<ExtensionProbe>(infos[i]);probes[i]=pass.get();
        pipeline.addPass(infos[i].slot,std::move(pass));
    }
    probes[1]->failSubmit=true;
    auto source=FgResourceId::FinalLdrColor;
    CHECK(declarePresentationStages(graph,stages,source));
    graph.addPass({"Output",{source},{},true,true});
    graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
    CHECK(graph.compile());graph.markProduced(FgResourceId::FinalLdrColor);
    ayt::shader::ShaderResourcePool pool;RenderScene scene;FrameContext frame;
    std::unordered_map<uint64_t,GpuMesh> meshes;
    std::unordered_map<uint64_t,GpuTexture> textures;
    std::unordered_map<uint64_t,GpuMaterial> materials;
    PassExecContext execution{adapter,pool,scene,meshes,textures,materials,0,0,16,16,frame,0};
    execution.frameGraph=&graph;
    CHECK(pipeline.executeAll(execution)==2);
    CHECK(probes[0]->observedSource==80);CHECK(probes[1]->observedSource==81);
    CHECK(probes[2]->observedSource==81);
    CHECK_FALSE(graph.producedThisFrame(infos[1].output));
    CHECK(graph.resolveSemantic(FgSemantic::PresentSource).idx==83);
}

TEST_CASE(invalid_descriptions_are_rejected_before_any_stage_is_declared)
{
    BGFXAdapter adapter;FrameGraph graph(adapter);
    std::array<PresentationStageFrame,2> stages;
    stages[0].info=&kPresentationStages[1];stages[1].info=&kPresentationStages[0];
    graph.beginFrame(16,16);auto source=FgResourceId::FinalLdrColor;
    CHECK_FALSE(declarePresentationStages(graph,stages,source));
    CHECK(validatePresentationStagePlan(stages,source).error==PresentationPlanError::InvalidOrder);
    CHECK(source==FgResourceId::FinalLdrColor);
    stages[1].info=stages[0].info;
    CHECK_FALSE(declarePresentationStages(graph,stages,source));
    stages[1].info=nullptr;
    CHECK_FALSE(declarePresentationStages(graph,stages,source));
    CHECK(validatePresentationStagePlan(stages,source).error==PresentationPlanError::MissingDescriptor);
    CHECK(graph.stats().physicalTargets==0);
}

TEST_CASE(reused_frame_storage_clears_imports_targets_and_old_pipeline_instances)
{
    const auto info=probeInfo(200,"Reusable",FgResourceId::ColorGradedColor,
        RenderPassResourceId::ColorGradedColor,10);
    RenderPipeline pipeline;auto pass=std::make_unique<ExtensionProbe>(info);
    auto* probe=pass.get();pipeline.addPass(info.slot,std::move(pass));
    BGFXAdapter adapter;ayt::shader::ShaderResourcePool pool;FrameContext frame;
    PresentationFrameContext context{adapter,pool,frame,0,0,16,16,true};
    std::vector<PresentationStageFrame> storage;
    preparePresentationStages(pipeline,context,storage);
    CHECK(storage.size()==1);CHECK(probe->preparations==1);
    auto* original=storage.data();
    storage[0].imports.push_back({FgResourceId::GBufferNormal,{87}});
    storage[0].extraReads.push_back(FgResourceId::GBufferNormal);
    storage[0].historyTarget={88};
    const auto importCapacity=storage[0].imports.capacity();
    probe->selected=false;
    preparePresentationStages(pipeline,context,storage);
    CHECK(storage.data()==original);CHECK_FALSE(storage[0].enabled);
    CHECK(storage[0].imports.empty());CHECK(storage[0].extraReads.empty());
    CHECK(!bgfx::isValid(storage[0].historyTarget));
    CHECK(storage[0].imports.capacity()==importCapacity);
    CHECK(probe->preparations==1);
    pipeline.clear();CHECK(pipeline.presentationPasses().empty());
    preparePresentationStages(pipeline,context,storage);CHECK(storage.empty());
}

TEST_CASE(stage_diagnostics_identify_conflicting_outputs_and_imports)
{
    auto first=probeInfo(200,"First",FgResourceId::ColorGradedColor,
        RenderPassResourceId::ColorGradedColor,10);
    auto second=probeInfo(201,"Second",FgResourceId::StylizedColor,
        RenderPassResourceId::StylizedColor,20);
    std::array<PresentationStageFrame,2> stages;
    stages[0].info=&first;stages[1].info=&second;
    CHECK(validatePresentationStagePlan(stages,FgResourceId::FinalLdrColor).valid());
    second.slot=first.slot;
    auto error=validatePresentationStagePlan(stages,FgResourceId::FinalLdrColor);
    CHECK(error.error==PresentationPlanError::DuplicateSlot);CHECK(error.stageIndex==1);
    second.slot=static_cast<RenderPassSlot>(201);second.output=first.output;
    CHECK(validatePresentationStagePlan(stages,FgResourceId::FinalLdrColor).error
        ==PresentationPlanError::DuplicateOutput);
    second.output=FgResourceId::StylizedColor;
    second.view=first.view;
    CHECK(validatePresentationStagePlan(stages,FgResourceId::FinalLdrColor).error
        ==PresentationPlanError::DuplicateView);
    second.view=61;
    stages[0].imports.push_back({second.output,{89}});
    CHECK(validatePresentationStagePlan(stages,FgResourceId::FinalLdrColor).error
        ==PresentationPlanError::ImportOverwritesColor);
    stages[0].imports[0].resource=FgResourceId::Count;
    CHECK(validatePresentationStagePlan(stages,FgResourceId::FinalLdrColor).error
        ==PresentationPlanError::InvalidImport);
    stages[0].imports.clear();second.write.withDepth=true;
    CHECK(validatePresentationStagePlan(stages,FgResourceId::FinalLdrColor).error
        ==PresentationPlanError::InvalidOutputContract);
    for(const auto code:{PresentationPlanError::MissingDescriptor,PresentationPlanError::InvalidOutput,
        PresentationPlanError::DuplicateSlot,PresentationPlanError::DuplicateOutput,PresentationPlanError::DuplicateView,
        PresentationPlanError::InvalidOrder,PresentationPlanError::InvalidOutputContract,
        PresentationPlanError::InvalidImport,PresentationPlanError::ImportOverwritesColor}) {
        CHECK(std::string_view(presentationPlanErrorName(code))!="unknown-presentation-error");
    }
}

TEST_CASE(all_main_view_names_have_contract_owners_and_ui_keeps_the_final_rank)
{
    for(const auto& view:kOrderedRenderViews) {
        CHECK(view.view<kRenderViewCapacity);
        if(view.owner.empty())continue;
        bool found=false;
        for(uint16_t raw=0;raw<256;++raw) {
            if(const auto* contract=renderPassContract(static_cast<RenderPassSlot>(raw)))
                found|=contract->name==view.owner;
        }
        CHECK(found);CHECK(renderViewOwnerName(view.view)==view.owner);
    }
    CHECK(renderViewOwnerName(255)=="UI");
    CHECK(kRenderViewRemap.back()==255);
    for(uint16_t view=kCameraFirstView;view<=kCameraReadbackView;++view) {
        CHECK(renderViewOwnerName(view).empty());
        CHECK(std::find(kRenderViewOrder.begin(),kRenderViewOrder.end(),view)==kRenderViewOrder.end());
    }
    for(uint16_t view:{55,56,57,58,59,60,61,62,63,113,114})
        CHECK(renderViewOwnerName(view).empty());
}

TEST_CASE(prepared_contributions_are_authoritative_and_invalid_plans_fail_when_disabled)
{
    BGFXAdapter adapter;FrameGraph graph(adapter);
    PresentationStageFrame stage;stage.info=presentationStageInfo(RenderPassSlot::ColorGrading);
    auto input=inputForAa();
    const auto empty=buildPostProcessGraphPlan(graph,input);
    CHECK(empty.compileSucceeded);CHECK(empty.presentationSource==FgResourceId::FinalLdrColor);
    for(const auto& info:kPresentationStages)CHECK_FALSE(graph.shouldExecute(info.slot));
    input.presentationStages={&stage,1};
    const auto result=buildPostProcessGraphPlan(graph,input);
    CHECK(result.compileSucceeded);CHECK(graph.stats().livePasses==2);
    CHECK(graph.stats().logicalResources==2);
    CHECK_FALSE(graph.shouldExecute(RenderPassSlot::StylizedOutline));
    CHECK_FALSE(graph.shouldExecute(RenderPassSlot::AnalogDisplay));
    stage.info=nullptr;input.finalLdr=false;
    const auto invalid=buildPostProcessGraphPlan(graph,input);
    CHECK_FALSE(invalid.compileSucceeded);
    CHECK(invalid.presentationValidation.error==PresentationPlanError::MissingDescriptor);
}

TEST_CASE(stage_slot_mismatch_is_rejected_without_changing_the_live_indexes)
{
    const auto info=probeInfo(200,"OwnSlot",FgResourceId::ColorGradedColor,
        RenderPassResourceId::ColorGradedColor,10);
    RenderPipeline pipeline;
    pipeline.addPass(std::make_unique<ExtensionProbe>(info));
    CHECK(pipeline.passes().size()==1);CHECK(pipeline.presentationPasses().size()==1);
    auto* original=pipeline.presentationPasses().front();
    bool rejected=false;
    try {pipeline.addPass(RenderPassSlot::UI,std::make_unique<ExtensionProbe>(info));}
    catch(const std::invalid_argument&) {rejected=true;}
    CHECK(rejected);CHECK(pipeline.passes().size()==1);
    CHECK(pipeline.presentationPasses().size()==1);
    CHECK(pipeline.presentationPasses().front()==original);
    CHECK(pipeline.findPass<ExtensionProbe>()==original);
}

TEST_CASE(terminal_integration_saves_one_target_without_changing_the_logical_chain)
{
    BGFXAdapter adapter;FrameGraph graph(adapter);
    CHECK_FALSE(adapter.supportsDirectPresentation());
    CHECK(RenderPipelineDesc::makeDefault().integratePresentation);
    for(int aa=0;aa<4;++aa)for(unsigned mask=0;mask<(1u<<kPresentationStages.size());++mask) {
        auto input=inputForAa(aa);
        std::vector<PresentationStageFrame> stages;
        for(size_t i=0;i<kPresentationStages.size();++i) {
            PresentationStageFrame stage;stage.info=&kPresentationStages[i];stage.enabled=mask&(1u<<i);
            if(stage.info->history)stage.historyTarget={76};
            stages.push_back(std::move(stage));
        }
        input.presentationStages=stages;
        const auto ordinary=buildPostProcessGraphPlan(graph,input);
        CHECK(ordinary.compileSucceeded);const auto stats=graph.stats();
        input.allowDirectPresentation=true;
        const auto integrated=buildPostProcessGraphPlan(graph,input);
        CHECK(integrated.compileSucceeded);
        CHECK(ordinary.presentationSource==integrated.presentationSource);
        CHECK(graph.stats().livePasses==stats.livePasses);
        const bool direct=(mask&(1u<<(kPresentationStages.size()-1)))!=0;
        CHECK(graph.isDirectPresentationOutput(FgResourceId::DisplayColor)==direct);
        CHECK(graph.stats().physicalTargets==stats.physicalTargets-(direct?1:0));
        CHECK_FALSE(graph.directlyPresentedThisFrame());
        graph.markDirectlyPresented(FgResourceId::TemporalColor);
        CHECK_FALSE(graph.directlyPresentedThisFrame());
        graph.markDirectlyPresented(FgResourceId::DisplayColor);
        CHECK(graph.directlyPresentedThisFrame()==direct);
        if(direct) {
            CHECK(graph.producedThisFrame(FgResourceId::DisplayColor));
            CHECK_FALSE(BGFXAdapter::isValid(graph.resolve(FgResourceId::DisplayColor)));
        }
        graph.beginFrame(319,181);
        CHECK_FALSE(graph.directlyPresentedThisFrame());
        CHECK_FALSE(graph.isDirectPresentationOutput(FgResourceId::DisplayColor));
    }
}

TEST_CASE(terminal_integration_never_bypasses_a_later_color_consumer_or_history_target)
{
    BGFXAdapter adapter;FrameGraph graph(adapter);
    auto info=probeInfo(200,"AfterDisplay",FgResourceId::HazeColor,
        RenderPassResourceId::HazeColor,60);
    std::array<PresentationStageFrame,2> stages;
    stages[0].info=presentationStageInfo(RenderPassSlot::AnalogDisplay);stages[0].enabled=true;
    stages[1].info=&info;stages[1].enabled=true;
    auto input=inputForAa();input.presentationStages=stages;input.allowDirectPresentation=true;
    CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
    CHECK_FALSE(graph.isDirectPresentationOutput(FgResourceId::DisplayColor));
    CHECK_FALSE(graph.isDirectPresentationOutput(info.output));
    stages[1].enabled=false;
    CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
    CHECK(graph.isDirectPresentationOutput(FgResourceId::DisplayColor));
    graph.markDirectlyPresented(FgResourceId::DisplayColor);
    graph.resize(17,11);CHECK_FALSE(graph.directlyPresentedThisFrame());
    CHECK_FALSE(graph.isDirectPresentationOutput(FgResourceId::DisplayColor));
    info.history=true;info.canPresentDirectly=true;
    info.write.lifetime=RenderPassResourceLifetime::PersistentHistory;
    const auto invalid=buildPostProcessGraphPlan(graph,input);
    CHECK_FALSE(invalid.compileSucceeded);
    CHECK(invalid.presentationValidation.error==PresentationPlanError::InvalidOutputContract);
    graph.shutdown();CHECK_FALSE(graph.directlyPresentedThisFrame());
}

TEST_CASE(shared_extra_imports_must_agree_and_cannot_replace_upstream_declarations)
{
    auto first=probeInfo(200,"First",FgResourceId::ColorGradedColor,
        RenderPassResourceId::ColorGradedColor,10);
    auto second=probeInfo(201,"Second",FgResourceId::StylizedColor,
        RenderPassResourceId::StylizedColor,20);
    std::array<PresentationStageFrame,2> stages;
    stages[0].info=&first;stages[1].info=&second;
    for(auto& stage:stages) {
        stage.enabled=true;stage.imports.push_back({FgResourceId::FeedbackHistory,{81}});
        stage.extraReads.push_back(FgResourceId::FeedbackHistory);
    }
    auto input=inputForAa();input.presentationStages=stages;
    BGFXAdapter adapter;FrameGraph graph(adapter);
    CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
    stages[1].imports[0].framebuffer={82};
    auto invalid=buildPostProcessGraphPlan(graph,input);
    CHECK_FALSE(invalid.compileSucceeded);
    CHECK(invalid.presentationValidation.error==PresentationPlanError::ConflictingImport);
    CHECK(invalid.presentationValidation.stageIndex==1);
    CHECK(invalid.presentationValidation.resource==FgResourceId::FeedbackHistory);
    stages[1].imports[0].framebuffer={81};
    stages[0].imports[0]={FgResourceId::SceneColor,{99}};
    invalid=buildPostProcessGraphPlan(graph,input);
    CHECK_FALSE(invalid.compileSucceeded);
    CHECK(invalid.presentationValidation.error==PresentationPlanError::ConflictingImport);
    CHECK(invalid.presentationValidation.stageIndex==0);
    CHECK(invalid.presentationValidation.resource==FgResourceId::SceneColor);
    CHECK(graph.resolve(FgResourceId::SceneColor).idx==input.sceneColor.idx);
    stages[0].imports[0].framebuffer=input.sceneColor;
    stages[0].extraReads[0]=FgResourceId::SceneColor;
    CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
    CHECK(graph.resolve(FgResourceId::SceneColor).idx==input.sceneColor.idx);
}

TEST_CASE(output_identity_cannot_redeclare_an_upstream_owned_resource)
{
    auto info=probeInfo(200,"ConflictingOutput",FgResourceId::BloomBright,
        RenderPassResourceId::BloomBright,10);
    PresentationStageFrame stage;stage.info=&info;stage.enabled=true;
    auto input=inputForAa();input.bloomExtract=true;input.presentationStages={&stage,1};
    BGFXAdapter adapter;FrameGraph graph(adapter);
    const auto invalid=buildPostProcessGraphPlan(graph,input);
    CHECK_FALSE(invalid.compileSucceeded);
    CHECK(invalid.presentationValidation.error==PresentationPlanError::OutputAlreadyDeclared);
    CHECK(invalid.presentationValidation.stageIndex==0);
    CHECK(graph.resourceDeclared(FgResourceId::BloomBright));
    CHECK_FALSE(graph.canImportExternal(FgResourceId::BloomBright,{99}));
    CHECK_FALSE(graph.canImportExternal(static_cast<FgResourceId>(255),{99}));
    CHECK_FALSE(graph.resourceDeclared(static_cast<FgResourceId>(255)));
    CHECK(graph.canImportExternal(FgResourceId::FeedbackHistory,{99}));
}

TEST_CASE(preparation_exceptions_discard_partial_contributions_and_do_not_block_following_stages)
{
    std::array<PresentationStageInfo,3> infos{
        probeInfo(200,"ThrowsStandard",FgResourceId::ColorGradedColor,RenderPassResourceId::ColorGradedColor,10),
        probeInfo(201,"ThrowsOther",FgResourceId::StylizedColor,RenderPassResourceId::StylizedColor,20),
        probeInfo(202,"Ready",FgResourceId::DisplayColor,RenderPassResourceId::DisplayColor,30)};
    RenderPipeline pipeline;std::array<ExtensionProbe*,3> probes;
    for(size_t i=0;i<infos.size();++i) {
        auto pass=std::make_unique<ExtensionProbe>(infos[i]);probes[i]=pass.get();
        pipeline.addPass(infos[i].slot,std::move(pass));
    }
    probes[0]->throwPrepare=1;probes[1]->throwPrepare=2;
    BGFXAdapter adapter;ayt::shader::ShaderResourcePool pool;FrameContext frame;
    PresentationFrameContext context{adapter,pool,frame,0,0,16,16,true};
    std::vector<PresentationStageFrame> stages;
    preparePresentationStages(pipeline,context,stages);
    for(size_t i=0;i<2;++i) {
        CHECK_FALSE(stages[i].enabled);CHECK(stages[i].extraReads.empty());CHECK(stages[i].imports.empty());
        CHECK_FALSE(bgfx::isValid(stages[i].historyTarget));CHECK(stages[i].info==&infos[i]);
        CHECK(probes[i]->invalidations==1);CHECK(probes[i]->lastReason==ResourceInvalidationReason::PreparationFailed);
    }
    CHECK(stages[2].enabled);CHECK(probes[2]->preparations==1);
    FrameGraph graph(adapter);auto input=inputForAa();input.presentationStages=stages;
    const auto plan=buildPostProcessGraphPlan(graph,input);
    CHECK(plan.compileSucceeded);CHECK(plan.presentationSource==infos[2].output);
    probes[0]->throwPrepare=0;probes[1]->throwPrepare=0;
    preparePresentationStages(pipeline,context,stages);
    for(size_t i=0;i<3;++i){CHECK(stages[i].enabled);CHECK(probes[i]->preparations==2);}
    input.presentationStages=stages;CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
}

TEST_CASE(main_view_reservations_reject_dynamic_camera_and_default_ui_collisions)
{
    CHECK(validMainRenderViews(kOrderedRenderViews));
    std::array<RenderViewInfo,1> candidate{{{60,"Extension"}}};
    CHECK(validMainRenderViews(candidate));
    for(const uint16_t view:{kCameraFirstView,kCameraLastView,kCameraReadbackView,kDefaultUiView,uint16_t(256)}) {
        candidate[0].view=view;CHECK_FALSE(validMainRenderViews(candidate));
    }
    const std::array<RenderViewInfo,2> duplicate{{{60,"One"},{60,"Two"}}};
    CHECK_FALSE(validMainRenderViews(duplicate));
    auto info=probeInfo(200,"Extension",FgResourceId::StylizedColor,
        RenderPassResourceId::StylizedColor,10);
    PresentationStageFrame frame;frame.info=&info;frame.enabled=true;
    for(uint8_t view:{uint8_t(0),uint8_t(16),uint8_t(kCameraFirstView),uint8_t(kCameraReadbackView),uint8_t(kDefaultUiView)}) {
        info.view=view;
        CHECK(validatePresentationStagePlan({&frame,1},FgResourceId::FinalLdrColor).error==PresentationPlanError::ReservedView);
    }
}

TEST_CASE(preparation_cannot_replace_identity_or_export_partial_unavailable_resources)
{
    const auto first=probeInfo(200,"Owner",FgResourceId::ColorGradedColor,
        RenderPassResourceId::ColorGradedColor,10);
    const auto other=probeInfo(201,"Other",FgResourceId::StylizedColor,
        RenderPassResourceId::StylizedColor,20);
    ExtensionProbe probe(first);probe.replaceInfo=&other;
    BGFXAdapter adapter;ayt::shader::ShaderResourcePool pool;FrameContext frame;
    PresentationFrameContext context{adapter,pool,frame,0,0,16,16,true};
    auto prepared=probe.preparePresentationFrame(context);
    CHECK(prepared.info==&first);CHECK(prepared.enabled);
    probe.partialPrepareFailure=true;
    probe.preparePresentationFrame(context,prepared);
    CHECK(prepared.info==&first);CHECK_FALSE(prepared.enabled);
    CHECK(prepared.extraReads.empty());CHECK(prepared.imports.empty());
    CHECK_FALSE(bgfx::isValid(prepared.historyTarget));
    FrameGraph graph(adapter);auto input=inputForAa();input.presentationStages={&prepared,1};
    CHECK(buildPostProcessGraphPlan(graph,input).compileSucceeded);
}

TEST_CASE(twenty_stage_toggle_sequences_keep_resource_and_dispatch_contracts_without_pairwise_adapters)
{
    constexpr size_t count=20;
    std::array<PresentationStageInfo,count> infos;
    std::array<PresentationStageFrame,count> stages;
    std::array<std::string,count> names;
    size_t output=0;
    for(size_t i=0;i<count;++i) {
        while(output<kFrameGraphResourceNumbers.size()
            && (output==static_cast<size_t>(FgResourceId::FinalLdrColor)
                || contractResourceFor(static_cast<FgResourceId>(output))==RenderPassResourceId::Count))++output;
        CHECK(output<kFrameGraphResourceNumbers.size());
        if(output>=kFrameGraphResourceNumbers.size())return;
        names[i]="Synthetic"+std::to_string(i);
        const auto resource=static_cast<FgResourceId>(output++);
        infos[i]=probeInfo(static_cast<uint8_t>(200+i),names[i].c_str(),
            resource,contractResourceFor(resource),
            static_cast<uint16_t>(10+i),i%3==0);
        stages[i].info=&infos[i];stages[i].historyTarget={static_cast<uint16_t>(100+i)};
        stages[i].imports.push_back({FgResourceId::FroxelHistoryWrite,{90}});
        stages[i].extraReads.push_back(FgResourceId::FroxelHistoryWrite);
    }
    BGFXAdapter adapter;FrameGraph graph(adapter);uint32_t random=0x631ba49;
    for(size_t frame=0;frame<256;++frame) {
        random^=random<<13;random^=random>>17;random^=random<<5;
        const uint32_t mask=frame==0?0:frame==1?(1u<<count)-1:random;
        graph.beginFrame(static_cast<uint16_t>(319+frame%31),181);
        graph.importExternal(FgResourceId::FinalLdrColor,{80});
        auto expected=FgResourceId::FinalLdrColor;unsigned enabled=0,owned=0;
        for(size_t i=0;i<count;++i) {
            stages[i].enabled=(mask&(1u<<i))!=0;
            if(stages[i].enabled) {expected=infos[i].output;++enabled;if(!infos[i].history)++owned;}
        }
        auto source=FgResourceId::FinalLdrColor;
        CHECK(declarePresentationStages(graph,stages,source));CHECK(source==expected);
        graph.addPass({"Present",{source},{},true,true});
        graph.setResolvedSemantic(FgSemantic::PresentSource,FgResourceId::FinalLdrColor);
        CHECK(graph.compile());CHECK(graph.stats().livePasses==enabled+1);
        CHECK(graph.stats().physicalTargets==owned);
        CHECK(graph.stats().logicalResources==enabled+1+(enabled?1:0));
        for(size_t i=0;i<count;++i) {
            CHECK(graph.shouldExecute(infos[i].slot)==stages[i].enabled);
            if(!stages[i].enabled)CHECK_FALSE(BGFXAdapter::isValid(graph.resolve(infos[i].output)));
        }
    }
}

TEST_CASE(resource_declarations_cover_both_domains_without_conflating_distinct_boundaries)
{
    CHECK(denseResourceNumbers(kFrameGraphResourceNumbers));
    CHECK(denseResourceNumbers(kPassContractResourceNumbers));
    for(size_t index=0;index<static_cast<size_t>(FgResourceId::Count);++index) {
        const auto id=static_cast<FgResourceId>(index);
        CHECK(std::string_view(frameGraphResourceName(id))!="UnknownResource");
        const auto mapped=contractResourceFor(id);
        if(mapped!=RenderPassResourceId::Count)CHECK(frameGraphResourceFor(mapped)==id);
    }
    for(size_t index=0;index<static_cast<size_t>(RenderPassResourceId::Count);++index) {
        const auto id=static_cast<RenderPassResourceId>(index);
        CHECK(std::string_view(renderPassResourceName(id))!="UnknownResource");
        const auto mapped=frameGraphResourceFor(id);
        if(mapped!=FgResourceId::Count)CHECK(contractResourceFor(mapped)==id);
    }
    CHECK(contractResourceFor(FgResourceId::SSAOTexture)==RenderPassResourceId::SSAO);
    CHECK(frameGraphResourceFor(RenderPassResourceId::SSAO)==FgResourceId::SSAOTexture);
    CHECK(std::string_view(frameGraphResourceName(FgResourceId::SSAOTexture))=="SSAOTexture");
    CHECK(std::string_view(renderPassResourceName(RenderPassResourceId::SSAO))=="SSAO");
    CHECK(contractResourceFor(FgResourceId::BaseGBuffer)==RenderPassResourceId::Count);
    CHECK(frameGraphResourceFor(RenderPassResourceId::DecalGBuffer)==FgResourceId::Count);
    CHECK(contractResourceFor(FgResourceId::FroxelField)==RenderPassResourceId::Count);
    CHECK(std::string_view(frameGraphResourceName(FgResourceId::HazeHalf))=="HazeColor");
    CHECK(contractResourceFor(static_cast<FgResourceId>(255))==RenderPassResourceId::Count);
    CHECK(frameGraphResourceFor(static_cast<RenderPassResourceId>(255))==FgResourceId::Count);
    CHECK(std::string_view(frameGraphResourceName(static_cast<FgResourceId>(255)))=="UnknownResource");
    auto info=probeInfo(200,"WrongContract",FgResourceId::StylizedColor,RenderPassResourceId::DisplayColor,10);
    PresentationStageFrame stage;stage.info=&info;
    CHECK(validatePresentationStagePlan({&stage,1},FgResourceId::FinalLdrColor).error==PresentationPlanError::InvalidOutputContract);
    info.write.resource=contractResourceFor(info.output);
    CHECK(validatePresentationStagePlan({&stage,1},FgResourceId::FinalLdrColor).valid());
}

TEST_SUITE_END
