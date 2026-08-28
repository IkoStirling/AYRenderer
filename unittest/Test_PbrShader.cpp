#include "AYRenderer/PbrShaderSources.h"

#include "AYShader/BGFXConverter.h"
#include "AYShader/Phoskia.h"
#include "AYTest.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{

std::string trimSingleBoundaryNewline(std::string value)
{
    if (!value.empty() && value.front() == '\n') {
        value.erase(value.begin());
    }
    if (!value.empty() && value.back() != '\n') {
        value.push_back('\n');
    }
    return value;
}

void compilePbr(const char* platform, const char* profile,
                ayt::shader::CompiledShaderProgram& result)
{
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
#ifdef AY_SHADER_SHADERC_HINT
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
#endif
    backend.platform = platform;
    backend.profile = profile;
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    compiler.compileToProgram(ayt::render::kPbrPhoskiaSource,
                              frontend, backend, result);
    if (!result.success) {
        std::cerr << "[runtime pbr " << platform << '/' << profile
                  << "] compile failed:\n";
        for (const std::string& error : result.errors) {
            std::cerr << "  " << error << '\n';
        }
    }
}

} // namespace

TEST_SUITE(AYPbrShader)

TEST_CASE(runtime_asset_matches_embedded_pbr_source)
{
    const std::string path =
        std::string(AY_RENDERER_SOURCE_DIR) + "/demo/assets/pbr.phoskia";
    std::ifstream file(path, std::ios::binary);
    CHECK(file.is_open());
    const std::string disk((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    CHECK(disk == trimSingleBoundaryNewline(ayt::render::kPbrPhoskiaSource));
}

TEST_CASE(runtime_pbr_declares_imported_material_contract)
{
    const std::string source(ayt::render::kPbrPhoskiaSource);
    CHECK(source.find("texture2d baseColorTexture") != std::string::npos);
    CHECK(source.find("texture2d opacityTexture") != std::string::npos);
    CHECK(source.find("texture2d normalTexture") != std::string::npos);
    CHECK(source.find("texture2d metallicTexture") != std::string::npos);
    CHECK(source.find("texture2d roughnessTexture") != std::string::npos);
    CHECK(source.find("texture2d aoTexture") != std::string::npos);
    CHECK(source.find("texture2d emissiveTexture") != std::string::npos);
    CHECK(source.find("texture2d shadowMap") != std::string::npos);
    CHECK(source.find("texturecube envCube") != std::string::npos);
    CHECK(source.find("uniformblock Lights") != std::string::npos);
    CHECK(source.find("vec4 dirs[8]") != std::string::npos);
    CHECK(source.find("uniform vec4 activeLightCount") != std::string::npos);
    CHECK(source.find("uniform vec4 shadowAtlasRects[8]") != std::string::npos);
    CHECK(source.find("uniform mat4 lightViewProjs[8]") != std::string::npos);
    CHECK(source.find("uniform vec4 perLightShadowCount") != std::string::npos);
    CHECK(source.find("property metallic") != std::string::npos);
    CHECK(source.find("property roughness") != std::string::npos);
    CHECK(source.find("property emissive") != std::string::npos);
    CHECK(source.find("property opacity") != std::string::npos);
    CHECK(source.find("property opacitySource") != std::string::npos);
    CHECK(source.find("mix(1.0, dedicatedOpacity") != std::string::npos);
    CHECK(source.find("step(1.5, opacitySource.x)") != std::string::npos);
    CHECK(source.find("fresnelSchlick") != std::string::npos);
    CHECK(source.find("distributionGGX") != std::string::npos);
    CHECK(source.find("geometrySmith") != std::string::npos);
    CHECK(source.find("modelViewProjection") != std::string::npos);
    CHECK(source.find("in tan : tangent") != std::string::npos);
    CHECK(source.find("normalYSign") != std::string::npos);
    CHECK(source.find("premultipliedAlpha") != std::string::npos);
    CHECK(source.find("let direct7 = (diffuse7 + specular7)")
          != std::string::npos);
    CHECK(source.find("let shadow7 = mix(") != std::string::npos);
    CHECK(source.find("let cubeAmbient = sample(envCube, N)")
          != std::string::npos);
}

TEST_CASE(runtime_pbr_frontend_and_shaderc_compile)
{
    ayt::shader::CompiledShaderProgram program;
    compilePbr("linux", "430", program);
    CHECK(program.success);
}

TEST_CASE(runtime_pbr_d3d_reflection_preserves_texture_binding_name)
{
    ayt::shader::CompiledShaderProgram program;
    compilePbr("windows", "s_5_0", program);
    CHECK(program.success);
    bool sawBaseColor = false;
    bool sawOpacity = false;
    for (const ayt::shader::BGFXTexture& texture : program.textures) {
        sawBaseColor = sawBaseColor || texture.name == "baseColorTexture";
        sawOpacity = sawOpacity || texture.name == "opacityTexture";
    }
    CHECK(sawBaseColor);
    CHECK(sawOpacity);
}

TEST_SUITE_END
