#include "detail/BuiltinShaderCatalog.h"
#include "detail/LightingPass.h"
#include "detail/RigidInstancing.h"
#include "detail/WorldLit2DMaterialSource.h"
#include "AYRenderer/AutoExposureShaderSources.h"
#include "AYRenderer/BloomShaderSources.h"
#include "AYRenderer/DepthHazeShaderSources.h"
#include "AYRenderer/PbrShaderSources.h"
#include "AYRenderer/ShadowShaderSources.h"
#include "AYRenderer/SSAOShaderSources.h"
#include "AYRenderer/SsrShaderSources.h"
#include "AYRenderer/VolumetricShaderSources.h"
#include "AYRenderer/DeferredDecalShaderSources.h"
#include "AYRenderer/ParticleDraw.h"
#include "AYRenderer/FallbackMeshShaderSources.h"
#include "AYRenderer/TilemapShaderSources.h"
namespace ayt::render::detail {
std::vector<shader::ShaderBuildJob> builtinShaderJobs() {
    std::vector<shader::ShaderBuildJob> jobs;
    appendGBufferPassShaders(jobs); appendColorGradingPassShaders(jobs);
    appendGBufferDebugPassShaders(jobs); appendMotionVectorPassShaders(jobs);
    appendPostProcessPassShaders(jobs); appendPresentPassShaders(jobs);
    appendSkyboxPassShaders(jobs); appendTAAPassShaders(jobs);
    appendTransparentPassShaders(jobs); appendFXAAPassShaders(jobs);
    appendSMAAPassShaders(jobs); appendEditorOverlayPassShaders(jobs); appendUiGpuContextShaders(jobs);
    for(uint32_t count=0;count<=kMaxSceneLights;++count) for(bool pcf : {false,true}) {
        auto job=embeddedShader("lighting",buildLightingVariantSource(count,pcf));
        job.variant="shadow"+std::to_string(count)+(pcf?"_pcf":"_hard"); jobs.push_back(std::move(job));
    }
    for(const auto& [name,source] : {
        std::pair{"auto_exposure",kAutoExposurePhoskiaSource},
        std::pair{"ssr",kSsrPhoskiaSource},
        std::pair{"volumetric",kVolumetricPhoskiaSource},
        std::pair{"deferred_decal",kDeferredDecalPhoskiaSource},
        std::pair{"particle_unlit",kParticlePhoskiaSource},
        std::pair{"bloom_extract",kBloomExtractPhoskiaSource},
        std::pair{"bloom_down",kBloomPyramidDownsamplePhoskiaSource},
        std::pair{"bloom_up",kBloomPyramidUpsamplePhoskiaSource},
        std::pair{"depth_haze",kDepthHazePhoskiaSource},std::pair{"ssao",kSsaoPhoskiaSource},
        std::pair{"pbr",kPbrPhoskiaSource},std::pair{"shadow",kShadowCasterPhoskiaSource},
        std::pair{"simple_lit",kSimpleLitShadowPhoskiaSource},
        std::pair{"world_lit_2d",kWorldLit2DMaterialPhoskiaSource},
        std::pair{"tilemap",kTilemapPhoskiaSource},std::pair{"sprite_retro",kSpriteRetroAaPhoskiaSource},
        std::pair{"tile_nearest",kTilemapChunkNearestPhoskiaSource},std::pair{"tile_linear",kTilemapChunkLinearPhoskiaSource},
        std::pair{"tile_4tap",kTilemapChunk4TapPhoskiaSource},std::pair{"tile_9tap",kTilemapChunk9TapPhoskiaSource},
        std::pair{"tile_semantic_shadow",kTilemapSemanticShadowPhoskiaSource}})
        jobs.push_back(embeddedShader(name,source));
    jobs.push_back(embeddedShader("pbr_instances",rigidInstancedSource(kPbrPhoskiaSource)));
    jobs.push_back(embeddedShader("shadow_instances",rigidInstancedSource(kShadowCasterPhoskiaSource,true)));
    jobs.push_back(rawShader("shadow_sc",kShadowCasterVertexSc,kShadowCasterFragmentSc,kShadowCasterVaryingSc));
    jobs.push_back(rawShader("shadow_mask",kShadowMaskCasterVertexSc,kShadowMaskCasterFragmentSc,kShadowMaskCasterVaryingSc));
    jobs.push_back(rawShader("simple_lit_sc",kSimpleLitShadowVertexSc,kSimpleLitShadowFragmentSc,kSimpleLitShadowVaryingSc));
    jobs.push_back(rawShader("rigid_skin_fallback",kRigidLitVertexSc,kSkinnedLitFragmentSc,kRigidLitVaryingDef));
    return jobs;
}
}
