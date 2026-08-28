#include "AYTest.h"

#include "AYRenderer.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/Ir.h"
#include "AYShader/Phoskia.h"

#include "detail/BGFXAdapter.h"
#include "detail/GBufferLayout.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/LightingPass.h"
#include "detail/RenderResourceManager.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

TEST_SUITE(AYRenderer_GBufferMaterialContract)

TEST_CASE(material_model_values_are_append_only)
{
    using ayt::render::MaterialModel;
    CHECK(static_cast<uint8_t>(MaterialModel::StandardLit) == 0u);
    CHECK(static_cast<uint8_t>(MaterialModel::Unlit) == 1u);
    CHECK(static_cast<uint8_t>(MaterialModel::Count) == 2u);
}

TEST_CASE(ao_and_material_model_round_trip_without_coverage)
{
    using ayt::render::MaterialModel;
    using ayt::render::detail::GBufferLayout;

    const float lit = GBufferLayout::packAoAndMaterialModel(
        0.37f, MaterialModel::StandardLit);
    const float unlit = GBufferLayout::packAoAndMaterialModel(
        0.81f, MaterialModel::Unlit);

    CHECK(GBufferLayout::unpackMaterialModel(lit)
          == MaterialModel::StandardLit);
    CHECK(GBufferLayout::unpackMaterialModel(unlit)
          == MaterialModel::Unlit);
    CHECK(std::abs(GBufferLayout::unpackAo(lit) - 0.37f) < 1.0e-6f);
    CHECK(std::abs(GBufferLayout::unpackAo(unlit) - 0.81f) < 1.0e-6f);
    CHECK(GBufferLayout::kWorldPositionPackedAttachment == 2u);
    CHECK(GBufferLayout::kEmissiveCoverageAttachment == 3u);
}

TEST_CASE(packed_contract_sanitizes_non_finite_values)
{
    using ayt::render::MaterialModel;
    using ayt::render::detail::GBufferLayout;

    const float packed = GBufferLayout::packAoAndMaterialModel(
        std::numeric_limits<float>::quiet_NaN(), MaterialModel::Unlit);
    CHECK(std::isfinite(packed));
    CHECK(GBufferLayout::unpackMaterialModel(packed) == MaterialModel::Unlit);
    CHECK(GBufferLayout::unpackAo(packed) == 1.0f);

    const float invalidPacked = std::numeric_limits<float>::infinity();
    CHECK(GBufferLayout::unpackMaterialModel(invalidPacked)
          == MaterialModel::StandardLit);
    CHECK(GBufferLayout::unpackAo(invalidPacked) == 0.0f);
}

TEST_CASE(gpu_material_defaults_to_standard_lit)
{
    const ayt::render::detail::GpuMaterial material;
    CHECK(material.materialModel == ayt::render::MaterialModel::StandardLit);
}

TEST_CASE(resource_manager_validates_and_updates_material_model)
{
    ayt::render::detail::BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool shaderPool;
    ayt::render::detail::RenderResourceManager resources(adapter, shaderPool);
    const ayt::render::MaterialHandle handle{41u};
    resources.materials().try_emplace(handle.id);

    CHECK(resources.setMaterialModel(
        handle, ayt::render::MaterialModel::Unlit));
    CHECK(resources.materials().at(handle.id).materialModel
          == ayt::render::MaterialModel::Unlit);
    CHECK_FALSE(resources.setMaterialModel(
        handle, ayt::render::MaterialModel::Count));
    CHECK_FALSE(resources.setMaterialModel(
        ayt::render::MaterialHandle{99u},
        ayt::render::MaterialModel::StandardLit));
}

TEST_CASE(public_material_model_setter_is_invalid_handle_safe)
{
    ayt::render::Renderer renderer;
    renderer.setMaterialModel({}, ayt::render::MaterialModel::Unlit);
    CHECK(true);
}

TEST_CASE(live_gbuffer_and_lighting_sources_generate_valid_ir)
{
    ayt::shader::phoskia::Compiler compiler;
    std::vector<std::string> errors;

    ayt::shader::phoskia::ir::IRProgram gbufferIr;
    CHECK(compiler.generateIr(
        ayt::render::detail::kGBufferPhoskiaSourceCStr,
        ayt::shader::phoskia::CompileOptions{}, gbufferIr, errors));
    CHECK(errors.empty());

    errors.clear();
    ayt::shader::phoskia::ir::IRProgram lightingIr;
    CHECK(compiler.generateIr(
        ayt::render::detail::kLightingPhoskiaSourceCStr,
        ayt::shader::phoskia::CompileOptions{}, lightingIr, errors));
    CHECK(errors.empty());
}

TEST_CASE(shader_contract_keeps_model_and_coverage_independent)
{
    const std::string gbuffer(
        ayt::render::detail::kGBufferPhoskiaSourceCStr);
    const std::string lighting(
        ayt::render::detail::kLightingPhoskiaSourceCStr);

    CHECK(gbuffer.find("property materialModel") != std::string::npos);
    CHECK(gbuffer.find("materialModel.x * 2.0 + materialAo")
          != std::string::npos);
    CHECK(gbuffer.find("gbufferMaterial = vec4(materialEmissive, 1.0)")
          != std::string::npos);

    CHECK(lighting.find("let materialModel = floor(worldSample.a * 0.5")
          != std::string::npos);
    CHECK(lighting.find("worldSample.a - materialModel * 2.0")
          != std::string::npos);
    CHECK(lighting.find("let coverage = step(0.5, surface.a)")
          != std::string::npos);
    CHECK(lighting.find("let isUnlit = step(0.5, materialModel)")
          != std::string::npos);
}

TEST_SUITE_END
