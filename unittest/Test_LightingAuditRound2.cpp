#include "AYTest.h"

#include "AYRenderer/RenderScene.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Ir.h"
#include "AYShader/Phoskia.h"

#include "detail/FgResource.h"
#include "detail/LightingPass.h"
#include "detail/ShadowPass.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{

std::size_t countSubstring(const std::string& text,
                           const std::string& needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

} // namespace

TEST_SUITE(AYRenderer_LightingAuditRound2)

TEST_CASE(scene_color_intermediates_use_hdr_format)
{
    CHECK(ayt::render::detail::kHdrSceneColorFormat
          == bgfx::TextureFormat::RGBA16F);
}

TEST_CASE(shadow_atlas_views_are_reserved_outside_main_pipeline_range)
{
    using ayt::render::detail::ShadowPass;
    CHECK(ShadowPass::kShadowAtlasFirstViewId == 18u);
    CHECK(ShadowPass::kShadowAtlasViewCount
          == ayt::render::detail::kShadowAtlasMaxSlots);
    CHECK(static_cast<uint16_t>(ShadowPass::kShadowAtlasFirstViewId)
              + ShadowPass::kShadowAtlasViewCount - 1u
          == 25u);
}

TEST_CASE(shadow_pass_default_sampling_contract_is_finite_and_full_rect)
{
    ayt::render::detail::ShadowPass pass;
    const float* rects = pass.shadowSampleRects();
    CHECK(pass.perLightShadowCount() == 0u);
    CHECK(pass.shadowProjectionCount() == 0u);
    CHECK(pass.shadowSampleMapSize()
          == ayt::render::detail::ShadowPass::kDefaultShadowMapSize);
    CHECK_FLOAT_EQ(rects[0], 0.0f, 1e-6f);
    CHECK_FLOAT_EQ(rects[1], 0.0f, 1e-6f);
    CHECK_FLOAT_EQ(rects[2], 1.0f, 1e-6f);
    CHECK_FLOAT_EQ(rects[3], 1.0f, 1e-6f);
}

TEST_CASE(lighting_shadow_variants_remove_inactive_sampling_work)
{
    using ayt::render::detail::buildLightingVariantSource;

    const std::string noShadow = buildLightingVariantSource(0u, false);
    const std::string threeHard = buildLightingVariantSource(3u, false);
    const std::string threePcf = buildLightingVariantSource(3u, true);
    const std::string allPcf = buildLightingVariantSource(
        ayt::render::kMaxSceneLights, true);

    CHECK(countSubstring(noShadow, "sample(shadowMap") == 0u);
    CHECK(noShadow.find("let _world = vec4(worldPos, 1.0)")
          == std::string::npos);
    CHECK(countSubstring(threeHard, "sample(shadowMap") == 3u);
    CHECK(countSubstring(threePcf, "sample(shadowMap") == 27u);
    CHECK(countSubstring(allPcf, "sample(shadowMap") == 72u);
    CHECK(threePcf.find("slot 3 (compile-time inactive)")
          != std::string::npos);
    CHECK(threePcf.find("let perLightShadow3 = vec4(1.0)")
          != std::string::npos);
    CHECK(allPcf.find("_globalKeyClip") == std::string::npos);
}

TEST_CASE(lighting_variants_generate_valid_phoskia_ir)
{
    struct Variant {
        uint32_t shadowCount;
        bool pcfEnabled;
    };
    const Variant variants[] = {
        {0u, false},
        {3u, false},
        {3u, true},
        {ayt::render::kMaxSceneLights, true},
    };

    ayt::shader::phoskia::Compiler compiler;
    for (const Variant variant : variants) {
        const std::string source =
            ayt::render::detail::buildLightingVariantSource(
                variant.shadowCount, variant.pcfEnabled);
        ayt::shader::phoskia::ir::IRProgram ir;
        std::vector<std::string> errors;
        CHECK(compiler.generateIr(
            source,
            ayt::shader::phoskia::CompileOptions{},
            ir,
            errors));
        CHECK(errors.empty());
    }
}

TEST_CASE(csm_lighting_variant_compiles_for_d3d11_and_d3d12_shader_model)
{
    const std::string source =
        ayt::render::detail::buildLightingVariantSource(3u, true);
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
#ifdef AY_SHADER_SHADERC_HINT
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
#endif
    backend.platform = "windows";
    backend.profile = "s_5_0";
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    ayt::shader::CompiledShaderProgram program;
    compiler.compileToProgram(source, frontend, backend, program);
    if (!program.success) {
        for (const std::string& error : program.errors) {
            std::cerr << "[CSM lighting shader] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(!program.vsBin.empty());
    CHECK(!program.fsBin.empty());
}

TEST_CASE(lighting_source_pins_active_count_safe_normal_and_full_brdf)
{
    const std::string source(
        ayt::render::detail::kLightingPhoskiaSourceCStr);
    CHECK(source.find("uniform vec4 activeLightCount")
          != std::string::npos);
    CHECK(source.find("uniform vec4 shadowLightProjectionSlots[8]")
          != std::string::npos);
    CHECK(source.find("uniform vec4 shadowCascadeSplits[8]")
          != std::string::npos);
    CHECK(source.find("let active7 = step(7.5, activeLightCount.x)")
          != std::string::npos);
    CHECK(source.find("max(length(decodedN), 0.0001)")
          != std::string::npos);
    CHECK(source.find("let direct7 = (brdfDiffuse7 + brdfSpecular7) * f7")
          != std::string::npos);
    CHECK(source.find("let ambientF = fresnelSchlickRoughness")
          != std::string::npos);
    CHECK(source.find("let oneOverPi = 0.31830988618")
          != std::string::npos);
}

TEST_SUITE_END
