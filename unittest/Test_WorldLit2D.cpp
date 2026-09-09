#include "AYTest.h"

#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYRenderer/ShadowShaderSources.h"
#include "AYShader/BGFXConverter.h"
#include "AYShader/Ir.h"
#include "AYShader/Phoskia.h"
#include "AYShader/ShadercDriver.h"

#include "detail/Draw2D.h"
#include "detail/Forward2DOpaquePass.h"
#include "detail/GBufferPass.h"
#include "detail/WorldLit2DMaterialSource.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

void compileWorldLit2D(const char* source,
                       const char* platform,
                       const char* profile,
                       ayt::shader::CompiledShaderProgram& out)
{
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::CompileOptions frontend;
    ayt::shader::BGFXCompileOptions backend;
    backend.shadercPath = AY_SHADER_SHADERC_HINT;
    backend.platform = platform;
    backend.profile = profile;
#ifdef AY_SHADER_BGFX_COMMON_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
    backend.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
    compiler.compileToProgram(source, frontend, backend, out);
}

bool hasTextureBinding(const ayt::shader::CompiledShaderProgram& program,
                       const char* name)
{
    for (const ayt::shader::BGFXTexture& texture : program.textures) {
        if (texture.name == name) {
            return true;
        }
    }
    return false;
}

bool worldLit2DFileExists(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

} // namespace

TEST_SUITE(AYRenderer_WorldLit2D)

TEST_CASE(render_domain_and_alpha_mode_values_are_append_only)
{
    using ayt::render::Material2DAlphaMode;
    using ayt::render::RenderDomain2D;
    using ayt::render::TilemapSamplingQuality;
    using ayt::render::UvMapping2D;

    CHECK(static_cast<uint8_t>(RenderDomain2D::SceneOverlay) == 0u);
    CHECK(static_cast<uint8_t>(RenderDomain2D::WorldLit) == 1u);
    CHECK(static_cast<uint8_t>(RenderDomain2D::Count) == 2u);
    CHECK(static_cast<uint8_t>(Material2DAlphaMode::Opaque) == 0u);
    CHECK(static_cast<uint8_t>(Material2DAlphaMode::Cutout) == 1u);
    CHECK(static_cast<uint8_t>(Material2DAlphaMode::Blend) == 2u);
    CHECK(static_cast<uint8_t>(Material2DAlphaMode::Count) == 3u);
    CHECK(static_cast<uint8_t>(UvMapping2D::SourceRectBottomLeft) == 0u);
    CHECK(static_cast<uint8_t>(UvMapping2D::BakedAtlas) == 1u);
    CHECK(static_cast<uint8_t>(UvMapping2D::Count) == 2u);
    CHECK(static_cast<uint8_t>(TilemapSamplingQuality::Nearest) == 0u);
    CHECK(static_cast<uint8_t>(TilemapSamplingQuality::Linear) == 1u);
    CHECK(static_cast<uint8_t>(TilemapSamplingQuality::Tap4) == 2u);
    CHECK(static_cast<uint8_t>(TilemapSamplingQuality::Tap9) == 3u);
    CHECK(static_cast<uint8_t>(TilemapSamplingQuality::Count) == 4u);
}

TEST_CASE(material2d_defaults_are_safe_for_a_cutout_sprite)
{
    const ayt::render::Material2DDesc desc;
    CHECK_FALSE(desc.albedo.isValid());
    CHECK_FALSE(desc.normal.isValid());
    CHECK_FALSE(desc.roughnessMap.isValid());
    CHECK_FALSE(desc.emissiveMap.isValid());
    CHECK_FLOAT_EQ(desc.metallic, 0.0f, 0.0f);
    CHECK_FLOAT_EQ(desc.roughness, 0.75f, 0.0f);
    CHECK_FLOAT_EQ(desc.ambientOcclusion, 1.0f, 0.0f);
    CHECK_FLOAT_EQ(desc.emissiveStrength, 0.0f, 0.0f);
    CHECK_FLOAT_EQ(desc.alphaCutoff, 0.5f, 0.0f);
    CHECK(desc.alphaMode == ayt::render::Material2DAlphaMode::Cutout);
    CHECK_FALSE(desc.invertNormalY);
    CHECK_TRUE(desc.doubleSided);
}

TEST_CASE(payload_defaults_to_overlay_and_routes_exclusively)
{
    ayt::render::DrawPayload2D overlayPayload;
    ayt::render::DrawPayload2D worldPayload;
    worldPayload.renderDomain = ayt::render::RenderDomain2D::WorldLit;

    CHECK(overlayPayload.uvMapping
          == ayt::render::UvMapping2D::SourceRectBottomLeft);
    CHECK(overlayPayload.samplingQuality
          == ayt::render::TilemapSamplingQuality::Linear);

    ayt::render::DrawItem overlay;
    overlay.payload = &overlayPayload;
    ayt::render::DrawItem world;
    world.payload = &worldPayload;

    CHECK(ayt::render::detail::isOverlay2DItem(overlay));
    CHECK_FALSE(ayt::render::detail::isWorldLit2DItem(overlay));
    CHECK_FALSE(ayt::render::detail::isOverlay2DItem(world));
    CHECK(ayt::render::detail::isWorldLit2DItem(world));

    ayt::render::RenderScene scene;
    scene.add(overlay);
    scene.add(world);
    const auto sorted = ayt::render::detail::collectSortedOverlay2DItems(scene);
    CHECK(sorted.size() == 1u);
    CHECK(sorted[0] == &scene.items()[0]);
}

TEST_CASE(world_lit_2d_gbuffer_source_generates_valid_ir)
{
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::ir::IRProgram ir;
    std::vector<std::string> errors;
    const bool success = compiler.generateIr(
        ayt::render::detail::kWorldLit2DGBufferPhoskiaSourceCStr,
        ayt::shader::phoskia::CompileOptions{}, ir, errors);
    CHECK(success);
    CHECK(errors.empty());
}

TEST_CASE(world_lit_2d_material_source_generates_valid_ir)
{
    ayt::shader::phoskia::Compiler compiler;
    ayt::shader::phoskia::ir::IRProgram ir;
    std::vector<std::string> errors;
    const bool success = compiler.generateIr(
        ayt::render::detail::kWorldLit2DMaterialPhoskiaSource,
        ayt::shader::phoskia::CompileOptions{}, ir, errors);
    CHECK(success);
    CHECK(errors.empty());
}

TEST_CASE(world_lit_2d_shaderc_compiles_gl_and_d3d_variants)
{
    for (const auto& target : {
             std::pair<const char*, const char*>{"linux", "430"},
             std::pair<const char*, const char*>{"windows", "s_5_0"}}) {
        ayt::shader::CompiledShaderProgram program;
        compileWorldLit2D(
            ayt::render::detail::kWorldLit2DGBufferPhoskiaSourceCStr,
            target.first, target.second, program);
        if (!program.success) {
            for (const std::string& error : program.errors) {
                std::cerr << "[WorldLit2D " << target.first << "] "
                          << error << '\n';
            }
        }
        CHECK(program.success);
        CHECK(!program.vsBin.empty());
        CHECK(!program.fsBin.empty());
    }
}

TEST_CASE(material2d_backing_program_retains_all_map_bindings)
{
    ayt::shader::CompiledShaderProgram program;
    compileWorldLit2D(
        ayt::render::detail::kWorldLit2DMaterialPhoskiaSource,
        "linux", "430", program);
    if (!program.success) {
        for (const std::string& error : program.errors) {
            std::cerr << "[Material2D backing] " << error << '\n';
        }
    }
    CHECK(program.success);
    CHECK(hasTextureBinding(program, "albedoMap"));
    CHECK(hasTextureBinding(program, "normalMap"));
    CHECK(hasTextureBinding(program, "roughnessMap"));
    CHECK(hasTextureBinding(program, "emissiveMap"));
}

TEST_CASE(public_material2d_creation_closes_on_noop_backend)
{
    ayt::render::Renderer renderer;
    ayt::render::InitDesc init;
    init.backend = ayt::render::Backend::Noop;
    init.width = 64;
    init.height = 64;
    CHECK(renderer.initialize(init));

    const uint8_t white[4] = {255u, 255u, 255u, 255u};
    ayt::render::TextureHandle albedo =
        renderer.createTextureFromRgba8(1, 1, white, "world-lit-2d-white");
    CHECK(albedo.isValid());

    ayt::render::Material2DDesc desc;
    desc.albedo = albedo;
    ayt::render::MaterialHandle material =
        renderer.createMaterial2D(desc, "world-lit-2d-material-public-api");
    CHECK(material.isValid());

    renderer.destroyMaterial(material);
    renderer.destroyTexture(albedo);
    renderer.shutdown();
}

TEST_CASE(world_lit_2d_source_pins_mrt_cutout_and_normal_contract)
{
    const std::string source(
        ayt::render::detail::kWorldLit2DGBufferPhoskiaSourceCStr);
    CHECK(source.find("in pos : position") != std::string::npos);
    CHECK(source.find("in uv : texcoord") != std::string::npos);
    CHECK(source.find("in nrm : normal") == std::string::npos);
    CHECK(source.find("if (albedo.a < alphaCutoff.x) { discard }")
          != std::string::npos);
    CHECK(source.find("texture2d normalMap") != std::string::npos);
    CHECK(source.find("texture2d roughnessMap") != std::string::npos);
    CHECK(source.find("texture2d emissiveMap") != std::string::npos);
    CHECK(source.find("gbufferWorldPosition = vec4(worldPos, packedAoModel)")
          != std::string::npos);
    CHECK(source.find("mix(1.0, -1.0, flip.x)") != std::string::npos);
    CHECK(source.find("normalYSign.x * mix(1.0, -1.0, flip.y)")
          != std::string::npos);
    CHECK(source.find("property uvMapping") != std::string::npos);
    CHECK(source.find("property samplingQuality") != std::string::npos);
    CHECK(source.find("mix(sourceRectUv, uv") != std::string::npos);
    CHECK(source.find("samplingQuality.x > 1.5") != std::string::npos);
    CHECK(source.find("samplingQuality.x > 2.5") != std::string::npos);
}

TEST_CASE(world_lit_2d_shadow_mask_matches_gbuffer_coverage_contract)
{
    const std::string vertex(ayt::render::kShadowMaskCasterVertexSc);
    const std::string fragment(ayt::render::kShadowMaskCasterFragmentSc);
    CHECK(vertex.find("uniform vec4 srcRect") != std::string::npos);
    CHECK(vertex.find("uniform vec4 flip") != std::string::npos);
    CHECK(vertex.find("uniform vec4 uvMapping") != std::string::npos);
    CHECK(vertex.find("mix(sourceRectUv, a_texcoord0") != std::string::npos);
    CHECK(fragment.find("uniform vec4 tint") != std::string::npos);
    CHECK(fragment.find("uniform vec4 atlasTexel") != std::string::npos);
    CHECK(fragment.find("uniform vec4 samplingQuality") != std::string::npos);
    CHECK(fragment.find("samplingQuality.x > 1.5") != std::string::npos);
    CHECK(fragment.find("samplingQuality.x > 2.5") != std::string::npos);
    CHECK(fragment.find("baseColor.a * tint.a") != std::string::npos);
}

TEST_CASE(world_lit_2d_shadow_mask_compiles_for_d3d11_and_d3d12)
{
    if (!worldLit2DFileExists(AY_SHADER_SHADERC_HINT)) {
        std::cerr << "[WorldLit2D test] SKIP: shaderc unavailable.\n";
        return;
    }

    ayt::shader::AYShadercDriver driver(AY_SHADER_SHADERC_HINT);
    const auto compileStage = [&](const char* stage, const char* source) {
        ayt::shader::ShaderCompileRequest request;
        request.stage = stage;
        request.scSource = source;
        request.varyingdefSource = ayt::render::kShadowMaskCasterVaryingSc;
        request.platform = "windows";
        request.profile = "s_5_0";
        request.outputName = std::string("world_lit_2d_shadow_") + stage;
        request.timeoutMs = 30000;
#ifdef AY_SHADER_BGFX_COMMON_HINT
        request.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
        request.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
        const ayt::shader::ShaderCompileResult result = driver.compile(request);
        if (!result.ok) {
            std::cerr << "[WorldLit2D shadow] shaderc " << stage
                      << " failed: " << result.stderrText << '\n';
        }
        return result.ok;
    };

    CHECK(compileStage("vertex", ayt::render::kShadowMaskCasterVertexSc));
    CHECK(compileStage("fragment", ayt::render::kShadowMaskCasterFragmentSc));
}

TEST_CASE(uninitialized_renderer_rejects_material2d_without_dereference)
{
    ayt::render::Renderer renderer;
    ayt::render::Material2DDesc desc;
    CHECK_FALSE(renderer.createMaterial2D(desc, "uninitialized").isValid());
    CHECK_FALSE(renderer.loadTexture("missing.aytex", true).isValid());
}

TEST_SUITE_END
