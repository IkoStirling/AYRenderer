#include "AYRenderer/ShadowConfig.h"
#include "AYTest.h"
#include "AYShader/Phoskia.h"
#include "AYShader/ShaderProgram.h"

#include <fstream>
#include <iostream>
#include <string>

using ayt::render::kSimpleLitShadowPhoskiaSource;

TEST_SUITE(ShadowPhoskiaEmit)

TEST_CASE(dump_and_check_simple_lit_shadow_phoskia_emit)
{
    ayt::shader::phoskia::CompileOptions opts;
    opts.keepSources = true;
    opts.dumpIntermediate = true;
    opts.dumpDir = ayt::test::testTmpPath("phoskia_emit");

    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::CompiledShaderProgram prog;
    compiler.compileToProgram(kSimpleLitShadowPhoskiaSource, opts, prog);

    if (!prog.success) {
        std::cerr << "[phoskia emit] FAILED\n";
        for (const auto& e : prog.errors) {
            std::cerr << "  " << e << "\n";
        }
    }
    CHECK(prog.success);

    std::string vs;
    std::string fs;
    std::string varying;
    for (const auto& [k, v] : prog.sources) {
        std::cerr << "[phoskia emit] source key='" << k << "' bytes=" << v.size() << "\n";
        if (k.find("vs_") != std::string::npos || k.find("_vs") != std::string::npos) {
            vs = v;
        } else if (k.find("fs_") != std::string::npos || k.find("_fs") != std::string::npos) {
            fs = v;
        } else if (k.find("varying") != std::string::npos) {
            varying = v;
        }
    }
    // Fallback: dump all concatenated if keys unexpected.
    if (fs.empty()) {
        for (const auto& [k, v] : prog.sources) {
            fs += "// === " + k + " ===\n" + v + "\n";
        }
    }

    {
        std::ofstream f(opts.dumpDir + "/_concat_fs_check.sc", std::ios::binary);
        f << fs;
    }

    CHECK(fs.find("SAMPLER2D(albedoMap") != std::string::npos);
    CHECK(fs.find("SAMPLER2D(shadowMap") != std::string::npos);
    // Stage order must match FO: albedo=0, shadow=1
    CHECK(fs.find("SAMPLER2D(albedoMap, 0)") != std::string::npos);
    CHECK(fs.find("SAMPLER2D(shadowMap, 1)") != std::string::npos);

    CHECK(fs.find("u_lightViewProj") != std::string::npos);
    CHECK(fs.find("shadowPcf") != std::string::npos);
    CHECK(fs.find("mul(") != std::string::npos);

    // lightDir is uniform vec3 in Phoskia — must not disappear.
    CHECK(fs.find("lightDir") != std::string::npos);
    CHECK(fs.find("lightColor") != std::string::npos);
    // Guard converter / overload bugs:
    // 1) float props missing → `vec3 shadow = mix(1.0, ...)`
    // 2) mix arity-only pick → `float lit = mix(litColor, debugRgb, ...)`
    CHECK(fs.find("vec3 shadow") == std::string::npos);
    CHECK(fs.find("float shadow") != std::string::npos);
    CHECK(fs.find("float shadowFilt") != std::string::npos);
    CHECK(fs.find("float lit = mix") == std::string::npos);
    CHECK(fs.find("vec3 lit") != std::string::npos);
    // R8 path: sample().x, vec4 light ABI, no property initializers.
    CHECK(fs.find("unpackFloatFromRgba") == std::string::npos);
    CHECK(fs.find("uniform vec4 lightDir") != std::string::npos);
    CHECK(fs.find("uniform vec4 lightColor") != std::string::npos);
    CHECK(fs.find("uniform float shadowBias") == std::string::npos);
    CHECK(fs.find("shadowBias =") == std::string::npos);
    CHECK(fs.find("lightDir.xyz") != std::string::npos);

    if (!vs.empty()) {
        CHECK(vs.find("u_model") != std::string::npos);
        CHECK(vs.find("u_modelViewProj") != std::string::npos);
    }
}

TEST_SUITE_END
