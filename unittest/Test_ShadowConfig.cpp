#include "AYShadowConfig.h"

#include "AYTest.h"
#include "AYShaderResourcePool.h"

#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

using ayt::render::ShadowSettings;
using ayt::render::kSimpleLitShadowPhoskiaSource;
using ayt::render::predictShadowLitFactor;

TEST_SUITE(AYShadowConfig)

TEST_CASE(shadow_settings_near_far)
{
    CHECK(ShadowSettings::kDepthRange == 23.9f);
    CHECK(ShadowSettings::kFarPlane == 24.0f);
    CHECK(ShadowSettings::kNearPlane == 0.1f);
}

TEST_CASE(shadow_encode_and_compare_contact_shadow)
{
    const float cubeNdc01   = 0.510f;
    const float groundNdc01 = 0.518f;

    const float litAtGround =
        predictShadowLitFactor(groundNdc01, cubeNdc01);
    CHECK(litAtGround < 0.05f);
}

TEST_CASE(shadow_cleared_map_stays_lit)
{
    const float groundNdc01 = 0.5157f;
    const float lit = predictShadowLitFactor(groundNdc01, 1.0f);
    CHECK(lit > 0.99f);
}

TEST_CASE(build_stamp_phase6)
{
    CHECK(std::string_view(ShadowSettings::kPipelineBuildStamp)
          == std::string_view("v13-phase7-vec4-abi"));
}

TEST_CASE(simple_lit_shadow_phoskia_mirrors_verified_sc)
{
    // vec4 ABI + R8 + 3x3 PCF — must match verified hand .sc surface.
    const std::string src(kSimpleLitShadowPhoskiaSource);
    CHECK(src.find("uniform vec4 lightDir") != std::string::npos);
    CHECK(src.find("uniform vec4 lightColor") != std::string::npos);
    CHECK(src.find("uniform vec4 shadowMapTexel") != std::string::npos);
    CHECK(src.find("property shadowPcf") != std::string::npos);
    CHECK(src.find("shadowBias.x") != std::string::npos);
    CHECK(src.find("lightDir.xyz") != std::string::npos);
    CHECK(src.find("let inMap") != std::string::npos);
    CHECK(src.find("1.0 / 9.0") != std::string::npos);
    CHECK(src.find("unpackFloatFromRgba") == std::string::npos);
    CHECK(src.find("o00") != std::string::npos);
    CHECK(src.find("o22") != std::string::npos);
    CHECK(src.find("s11") != std::string::npos);
    std::size_t samples = 0;
    for (std::size_t pos = 0;;) {
        pos = src.find("sample(shadowMap", pos);
        if (pos == std::string::npos) {
            break;
        }
        ++samples;
        pos += 16;
    }
    CHECK(samples == 9u);
}

TEST_CASE(simple_lit_shadow_phoskia_frontend_compiles)
{
    // Frontend + converter smoke (Noop profile). Does not switch Editor
    // off sc-isolation; only proves the reverse-translated source parses.
    ayt::shader::ShaderResourcePool pool;
#ifdef AY_SHADER_SHADERC_HINT
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
#endif
    pool.bindRendererTypeForTests(
        /*bgfxRendererType=*/0 /*Noop*/,
        /*platform=*/"linux",
        /*profile=*/"430");

    const ayt::shader::ShaderResource res =
        pool.acquire(kSimpleLitShadowPhoskiaSource,
                     "simple_lit_shadow_phoskia_from_sc_v4");
    if (!res.isValid()) {
        std::cerr << "[shadow phoskia] acquire failed:\n";
        for (const std::string& err : pool.lastCompileErrors()) {
            std::cerr << "  " << err << "\n";
        }
    }
    CHECK(res.isValid());
}

TEST_SUITE_END
