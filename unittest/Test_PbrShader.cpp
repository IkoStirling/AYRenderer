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
    pool.bindRendererTypeForTests(
        /*bgfxRendererType=*/0,
        /*platform=*/"linux",
        /*profile=*/"430");

    const ayt::shader::ShaderResource resource =
        pool.acquire(ayt::render::kPbrPhoskiaSource,
                     "runtime_pbr_phoskia_v1");
    if (!resource.isValid()) {
        std::cerr << "[runtime pbr] acquire failed:\n";
        for (const std::string& error : pool.lastCompileErrors()) {
            std::cerr << "  " << error << '\n';
        }
    }
    CHECK(resource.isValid());
}

TEST_SUITE_END
