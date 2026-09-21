#include "detail/PostProcessGraphPlan.h"
#include "detail/RenderPassContract.h"

#include <cassert>
#include <utility>
#include <vector>

namespace ayt::render::detail
{
namespace
{

const RenderPassContract& contractFor(RenderPassSlot slot)
{
    const RenderPassContract* contract = renderPassContract(slot);
    assert(contract != nullptr);
    return *contract;
}

bgfx::TextureFormat::Enum toFgFormat(RenderPassResourceFormat format)
{
    switch (format) {
    case RenderPassResourceFormat::R8:
        return bgfx::TextureFormat::R8;
    case RenderPassResourceFormat::RG16F:
        return bgfx::TextureFormat::RG16F;
    case RenderPassResourceFormat::RGBA8:
        return bgfx::TextureFormat::RGBA8;
    case RenderPassResourceFormat::RGBA16F:
        return bgfx::TextureFormat::RGBA16F;
    case RenderPassResourceFormat::D24S8:
        return bgfx::TextureFormat::D24S8;
    case RenderPassResourceFormat::Unknown:
        break;
    }
    assert(false && "RenderPass contract has no FrameGraph texture format");
    return bgfx::TextureFormat::Unknown;
}

FgTextureScale toFgScale(RenderPassResourceExtent extent)
{
    switch (extent) {
    case RenderPassResourceExtent::Full:
    case RenderPassResourceExtent::Backbuffer:
        return FgTextureScale::Full;
    case RenderPassResourceExtent::Half:
        return FgTextureScale::Half;
    case RenderPassResourceExtent::Atlas:
        break;
    }
    assert(false && "Atlas output cannot be allocated by the post graph");
    return FgTextureScale::Full;
}

FgTextureDesc graphTextureDesc(RenderPassSlot slot,
                               RenderPassResourceId resource,
                               bool pointSampled = false)
{
    const RenderPassContract& contract = contractFor(slot);
    for (const RenderPassResourceWrite& write : contract.writes) {
        if (write.resource == resource) {
            assert(write.lifetime == RenderPassResourceLifetime::Transient);
            return {toFgFormat(write.format),
                    toFgScale(write.extent),
                    /*transient=*/true,
                    write.withDepth,
                    pointSampled};
        }
    }
    assert(false && "FrameGraph resource missing from RenderPass contract");
    return {};
}

const char* contractName(RenderPassSlot slot)
{
    return contractFor(slot).name.data();
}

bool contractSideEffect(RenderPassSlot slot)
{
    return contractFor(slot).sideEffect;
}

} // namespace

PostProcessGraphPlanResult buildPostProcessGraphPlan(
    FrameGraph& graph,
    const PostProcessGraphPlanInput& input)
{
    PostProcessGraphPlanResult result{};

    graph.beginFrame(input.width, input.height);
    graph.importExternal(FgResourceId::SceneColor, input.sceneColor);

    // SSAO is consumed by Lighting outside this MVP graph, so its semantic is
    // also a liveness root. Runtime eligibility already guarantees the full
    // GBuffer -> SSAO -> Lighting chain exists.
    if (input.ssao) {
        graph.addResource(FgResourceId::SSAOTexture,
                          graphTextureDesc(RenderPassSlot::SSAO,
                                           RenderPassResourceId::SSAO));
        graph.addPass({contractName(RenderPassSlot::SSAO),
                       {}, {FgResourceId::SSAOTexture}, true});
        graph.setResolvedSemantic(FgSemantic::SSAOSource,
                                  FgResourceId::SSAOTexture);
    }

    if (input.haze) {
        graph.addResource(FgResourceId::HazeColor,
                          graphTextureDesc(RenderPassSlot::DepthHaze,
                                           RenderPassResourceId::HazeColor));
        graph.addPass({contractName(RenderPassSlot::DepthHaze),
                       {FgResourceId::SceneColor},
                       {FgResourceId::HazeColor},
                       true});
        graph.setResolvedSemantic(FgSemantic::HazeSource,
                                  FgResourceId::HazeColor);
        result.hdrSceneSource = FgResourceId::HazeColor;
    }

    if (input.bloomExtract) {
        graph.addResource(FgResourceId::BloomBright,
                          graphTextureDesc(RenderPassSlot::BloomExtract,
                                           RenderPassResourceId::BloomBright));
        graph.addPass({contractName(RenderPassSlot::BloomExtract),
                       {result.hdrSceneSource},
                       {FgResourceId::BloomBright},
                       true});
    }
    if (input.bloomBlur) {
        graph.addResource(FgResourceId::BloomBlurA,
                          graphTextureDesc(RenderPassSlot::BloomBlur,
                                           RenderPassResourceId::BloomBlurA));
        graph.addResource(FgResourceId::BloomBlurB,
                          graphTextureDesc(RenderPassSlot::BloomBlur,
                                           RenderPassResourceId::BloomBlurB));
        graph.addPass({"BloomBlurH",
                       {FgResourceId::BloomBright},
                       {FgResourceId::BloomBlurA},
                       true});
        graph.addPass({"BloomBlurV",
                       {FgResourceId::BloomBlurA},
                       {FgResourceId::BloomBlurB},
                       true});
        graph.setResolvedSemantic(FgSemantic::BloomSource,
                                  FgResourceId::BloomBlurB);
    }

    // Runtime source selection may promote HazeSource after its submit, while
    // this semantic intentionally retains the stable external scene fallback.
    graph.setResolvedSemantic(FgSemantic::FinalColorSource,
                              FgResourceId::SceneColor);

    if (!input.finalLdr) {
        result.compileSucceeded = graph.compile();
        return result;
    }

    graph.addResource(FgResourceId::FinalLdrColor,
                      graphTextureDesc(RenderPassSlot::PostProcess,
                                       RenderPassResourceId::FinalLdrColor));
    std::vector<FgResourceId> postProcessReads{result.hdrSceneSource};
    if (input.bloomBlur) {
        postProcessReads.push_back(FgResourceId::BloomBlurB);
    }
    graph.addPass({contractName(RenderPassSlot::PostProcess),
                   std::move(postProcessReads),
                   {FgResourceId::FinalLdrColor},
                   true});

    if (input.taa) {
        graph.importExternal(FgResourceId::TaaColor, input.taaWriteTarget);
        graph.addPass({contractName(RenderPassSlot::TAA),
                       {FgResourceId::FinalLdrColor},
                       {FgResourceId::TaaColor},
                       true,
                       contractSideEffect(RenderPassSlot::TAA)});
    }
    if (input.fxaa) {
        graph.addResource(FgResourceId::FxaaColor,
                          graphTextureDesc(RenderPassSlot::FXAA,
                                           RenderPassResourceId::FxaaColor));
        graph.addPass({contractName(RenderPassSlot::FXAA),
                       {FgResourceId::FinalLdrColor},
                       {FgResourceId::FxaaColor},
                       true});
    }
    if (input.smaa) {
        graph.addResource(FgResourceId::SmaaEdges,
                          graphTextureDesc(
                              RenderPassSlot::SMAA,
                              RenderPassResourceId::SmaaEdges,
                              input.smaaIntermediatePointSampled));
        graph.addResource(FgResourceId::SmaaBlendWeights,
                          graphTextureDesc(
                              RenderPassSlot::SMAA,
                              RenderPassResourceId::SmaaBlendWeights,
                              input.smaaIntermediatePointSampled));
        graph.addResource(FgResourceId::SmaaColor,
                          graphTextureDesc(RenderPassSlot::SMAA,
                                           RenderPassResourceId::SmaaColor));
        graph.addPass({contractName(RenderPassSlot::SMAA),
                       {input.fxaa
                            ? FgResourceId::FxaaColor
                            : FgResourceId::FinalLdrColor},
                       {FgResourceId::SmaaEdges,
                        FgResourceId::SmaaBlendWeights,
                        FgResourceId::SmaaColor},
                       true});
    }

    // Public settings normally make these modes exclusive. Keeping an
    // explicit priority makes malformed/custom combinations deterministic and
    // lets graph liveness remove lower-priority disconnected branches.
    if (input.taa) {
        result.antialiasingSource = FgResourceId::TaaColor;
    } else if (input.smaa) {
        result.antialiasingSource = FgResourceId::SmaaColor;
    } else if (input.fxaa) {
        result.antialiasingSource = FgResourceId::FxaaColor;
    }

    result.presentationSource = result.antialiasingSource;
    if (input.colorGrading) {
        graph.addResource(FgResourceId::ColorGradedColor,
                          graphTextureDesc(
                              RenderPassSlot::ColorGrading,
                              RenderPassResourceId::ColorGradedColor));
        graph.addPass({contractName(RenderPassSlot::ColorGrading),
                       {result.antialiasingSource},
                       {FgResourceId::ColorGradedColor},
                       true});
        result.presentationSource = FgResourceId::ColorGradedColor;
    }

    graph.addPass({contractName(RenderPassSlot::Present),
                   {result.presentationSource}, {}, true,
                   contractSideEffect(RenderPassSlot::Present)});
    // Later stages promote PresentSource only after successful submission.
    graph.setResolvedSemantic(FgSemantic::PresentSource,
                              FgResourceId::FinalLdrColor);

    result.compileSucceeded = graph.compile();
    return result;
}

} // namespace ayt::render::detail
