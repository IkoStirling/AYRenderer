#include "AYRenderer.h"
#include "AYRenderer/TilemapShaderSources.h"
#include "AYTest.h"

#include <sys/stat.h>

#include <array>
#include <iostream>
#include <string>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace
{
bool tilemapShadercAvailable()
{
    struct stat st;
    return std::string(AY_SHADER_SHADERC_HINT).empty() == false
        && ::stat(AY_SHADER_SHADERC_HINT, &st) == 0;
}
}

TEST_SUITE(TilemapSamplingVariantTests)

TEST_CASE(QualitySelectorReturnsFourDistinctContracts)
{
    using namespace ayt::render;
    const char* nearest = tilemapChunkShaderSource(TilemapSamplingQuality::Nearest);
    const char* linear = tilemapChunkShaderSource(TilemapSamplingQuality::Linear);
    const char* tap4 = tilemapChunkShaderSource(TilemapSamplingQuality::Tap4);
    const char* tap9 = tilemapChunkShaderSource(TilemapSamplingQuality::Tap9);
    CHECK_TRUE(nearest != linear && linear != tap4 && tap4 != tap9);
    CHECK_TRUE(std::string(nearest).find("TilemapChunkNearest") != std::string::npos);
    CHECK_TRUE(std::string(linear).find("TilemapChunkLinear") != std::string::npos);
    CHECK_TRUE(std::string(tap4).find("TilemapChunk4Tap") != std::string::npos);
    CHECK_TRUE(std::string(tap9).find("TilemapChunk9Tap") != std::string::npos);
    CHECK_TRUE(std::string(nearest).find("atlasTexel") != std::string::npos);
    CHECK_TRUE(std::string(tap9).find("0.1111111") != std::string::npos);
}

TEST_CASE(AllChunkSamplingVariantsCompile)
{
    if (!tilemapShadercAvailable()) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width = 64u;
    desc.height = 64u;
    CHECK_TRUE(renderer.initialize(desc));

    const std::array<ayt::render::TilemapSamplingQuality, 4> qualities = {
        ayt::render::TilemapSamplingQuality::Nearest,
        ayt::render::TilemapSamplingQuality::Linear,
        ayt::render::TilemapSamplingQuality::Tap4,
        ayt::render::TilemapSamplingQuality::Tap9,
    };
    for (size_t i = 0; i < qualities.size(); ++i) {
        auto material = renderer.createMaterialFromPhoskia(
            ayt::render::tilemapChunkShaderSource(qualities[i]),
            "tilemap_sampling_variant_" + std::to_string(i));
        CHECK_TRUE(material.isValid());
        if (material.isValid()) renderer.destroyMaterial(material);
    }
    renderer.shutdown();
}

TEST_SUITE_END
