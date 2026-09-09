#include "detail/PostProcessGraphPlan.h"

#include <utility>
#include <vector>

namespace ayt::render::detail
{

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
                          {bgfx::TextureFormat::RGBA8,
                           FgTextureScale::Full,
                           /*transient=*/true,
                           /*withDepth=*/false});
        graph.addPass({"SSAO", {}, {FgResourceId::SSAOTexture}, true});
        graph.setResolvedSemantic(FgSemantic::SSAOSource,
                                  FgResourceId::SSAOTexture);
    }

    if (input.haze) {
        graph.addResource(FgResourceId::HazeColor,
                          {kHdrSceneColorFormat,
                           FgTextureScale::Full,
                           /*transient=*/true,
                           /*withDepth=*/false});
        graph.addPass({"DepthHaze",
                       {FgResourceId::SceneColor},
                       {FgResourceId::HazeColor},
                       true});
        graph.setResolvedSemantic(FgSemantic::HazeSource,
                                  FgResourceId::HazeColor);
        result.hdrSceneSource = FgResourceId::HazeColor;
    }

    if (input.bloomExtract) {
        graph.addResource(FgResourceId::BloomBright,
                          {kHdrSceneColorFormat,
                           FgTextureScale::Half,
                           /*transient=*/true,
                           /*withDepth=*/false});
        graph.addPass({"BloomExtract",
                       {result.hdrSceneSource},
                       {FgResourceId::BloomBright},
                       true});
    }
    if (input.bloomBlur) {
        graph.addResource(FgResourceId::BloomBlurA,
                          {kHdrSceneColorFormat,
                           FgTextureScale::Half,
                           /*transient=*/true,
                           /*withDepth=*/false});
        graph.addResource(FgResourceId::BloomBlurB,
                          {kHdrSceneColorFormat,
                           FgTextureScale::Half,
                           /*transient=*/true,
                           /*withDepth=*/false});
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
                      {bgfx::TextureFormat::RGBA8,
                       FgTextureScale::Full,
                       /*transient=*/true,
                       /*withDepth=*/false});
    std::vector<FgResourceId> postProcessReads{result.hdrSceneSource};
    if (input.bloomBlur) {
        postProcessReads.push_back(FgResourceId::BloomBlurB);
    }
    graph.addPass({"PostProcess",
                   std::move(postProcessReads),
                   {FgResourceId::FinalLdrColor},
                   true});

    if (input.taa) {
        graph.importExternal(FgResourceId::TaaColor, input.taaWriteTarget);
        graph.addPass({"TAA",
                       {FgResourceId::FinalLdrColor},
                       {FgResourceId::TaaColor},
                       true});
    }
    if (input.fxaa) {
        graph.addResource(FgResourceId::FxaaColor,
                          {bgfx::TextureFormat::RGBA8,
                           FgTextureScale::Full,
                           /*transient=*/true,
                           /*withDepth=*/false});
        graph.addPass({"FXAA",
                       {FgResourceId::FinalLdrColor},
                       {FgResourceId::FxaaColor},
                       true});
    }
    if (input.smaa) {
        graph.addResource(FgResourceId::SmaaEdges,
                          {bgfx::TextureFormat::RGBA8,
                           FgTextureScale::Full,
                           /*transient=*/true,
                           /*withDepth=*/false,
                           input.smaaIntermediatePointSampled});
        graph.addResource(FgResourceId::SmaaBlendWeights,
                          {bgfx::TextureFormat::RGBA8,
                           FgTextureScale::Full,
                           /*transient=*/true,
                           /*withDepth=*/false,
                           input.smaaIntermediatePointSampled});
        graph.addResource(FgResourceId::SmaaColor,
                          {bgfx::TextureFormat::RGBA8,
                           FgTextureScale::Full,
                           /*transient=*/true,
                           /*withDepth=*/false});
        graph.addPass({"SMAA",
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
                          {bgfx::TextureFormat::RGBA8,
                           FgTextureScale::Full,
                           /*transient=*/true,
                           /*withDepth=*/false});
        graph.addPass({"ColorGrading",
                       {result.antialiasingSource},
                       {FgResourceId::ColorGradedColor},
                       true});
        result.presentationSource = FgResourceId::ColorGradedColor;
    }

    graph.addPass({"Present", {result.presentationSource}, {}, true});
    // Later stages promote PresentSource only after successful submission.
    graph.setResolvedSemantic(FgSemantic::PresentSource,
                              FgResourceId::FinalLdrColor);

    result.compileSucceeded = graph.compile();
    return result;
}

} // namespace ayt::render::detail
