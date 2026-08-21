#include "AYRenderer/PbrShaderSources.h"

#include "AYShader/ShaderResourcePool.h"
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
    CHECK(source.find("texture2d shadowMap") != std::string::npos);
    CHECK(source.find("property metallic") != std::string::npos);
    CHECK(source.find("property roughness") != std::string::npos);
    CHECK(source.find("property emissive") != std::string::npos);
    CHECK(source.find("property opacity") != std::string::npos);
    CHECK(source.find("fresnelSchlick") != std::string::npos);
    CHECK(source.find("distributionGGX") != std::string::npos);
    CHECK(source.find("geometrySmith") != std::string::npos);
    CHECK(source.find("modelViewProjection") != std::string::npos);
}

TEST_CASE(runtime_pbr_frontend_and_shaderc_compile)
{
    ayt::shader::ShaderResourcePool pool;
#ifdef AY_SHADER_SHADERC_HINT
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
#endif
    std::vector<std::string> includeDirs;
#ifdef AY_SHADER_BGFX_COMMON_HINT
    includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    pool.setBgfxIncludeDirs(includeDirs);
    pool.bindRendererTypeForTests(
        /*bgfxRendererType=*/0,
        /*platform=*/"linux",
        /*profile=*/"430");

    const ayt::shader::ShaderResource resource =
        pool.acquire(ayt::render::kPbrPhoskiaSource,
                     "runtime_pbr_phoskia_v2_opacity");
    if (!resource.isValid()) {
        std::cerr << "[runtime pbr] acquire failed:\n";
        for (const std::string& error : pool.lastCompileErrors()) {
            std::cerr << "  " << error << '\n';
        }
    }
    CHECK(resource.isValid());
}

TEST_CASE(runtime_pbr_d3d_reflection_preserves_texture_binding_name)
{
    ayt::shader::ShaderResourcePool pool;
#ifdef AY_SHADER_SHADERC_HINT
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
#endif
    std::vector<std::string> includeDirs;
#ifdef AY_SHADER_BGFX_COMMON_HINT
    includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    pool.setBgfxIncludeDirs(includeDirs);
    pool.bindRendererTypeForTests(
        /*bgfxRendererType=*/0,
        /*platform=*/"windows",
        /*profile=*/"s_5_0");

    const ayt::shader::ShaderResource resource =
        pool.acquire(ayt::render::kPbrPhoskiaSource,
                     "runtime_pbr_phoskia_d3d_reflection_v2_opacity");
    if (!resource.isValid()) {
        std::cerr << "[runtime pbr d3d] acquire failed:\n";
        for (const std::string& error : pool.lastCompileErrors()) {
            std::cerr << "  " << error << '\n';
        }
    }
    CHECK(resource.isValid());
    CHECK(resource.getTextureBinding("baseColorTexture")
          != ayt::shader::InvalidBinding);
    CHECK(resource.getTextureBinding("opacityTexture")
          != ayt::shader::InvalidBinding);
}

TEST_SUITE_END
