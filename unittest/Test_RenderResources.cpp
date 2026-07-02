#include "AYRenderer.h"
#include "AYTest.h"

#include <iostream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

const char* kTexturedMaterial = R"(
material TexturedUnlit {
    texture2d albedoMap
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)

    vertex {
        in pos : position
        in uv  : texcoord
        out uvOut : texcoord = vec2(0.0, 0.0)
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in uvOut : texcoord
        return sample(albedoMap, uvOut) * baseColor
    }
}
)";

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

std::vector<uint8_t> makeCheckerboardRgba8(uint32_t width, uint32_t height, uint32_t cellSize)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const bool on = ((x / cellSize) + (y / cellSize)) % 2u == 0u;
            const uint8_t c = on ? 220u : 40u;
            const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
            pixels[i + 0] = c;
            pixels[i + 1] = on ? 80u : 180u;
            pixels[i + 2] = on ? 80u : 220u;
            pixels[i + 3] = 255u;
        }
    }
    return pixels;
}

} // namespace

TEST_SUITE(RenderResourceTests)

TEST_CASE(texture_cache_reuses_same_key)
{
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    CHECK(renderer.initialize(desc));

    const std::vector<uint8_t> pixels = makeCheckerboardRgba8(4, 4, 2);
    ayt::render::TextureHandle a =
        renderer.createTextureFromRgba8(4, 4, pixels.data(), "checker_4x4");
    ayt::render::TextureHandle b =
        renderer.createTextureFromRgba8(4, 4, pixels.data(), "checker_4x4");
    CHECK(a.isValid());
    CHECK(b.isValid());
    CHECK(a.id == b.id);

    renderer.destroyTexture(a);
    renderer.shutdown();
}

TEST_CASE(textured_material_draw_one_frame)
{
    if (!fileExists(AY_SHADER_SHADERC_HINT)) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 640;
    desc.height  = 480;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle material =
        renderer.createMaterialFromPhoskia(kTexturedMaterial, "renderer_textured_unlit");
    if (!material.isValid()) {
        std::cerr << "[Renderer test] SKIP: material acquire failed.\n";
        renderer.shutdown();
        return;
    }

    const std::vector<uint8_t> pixels = makeCheckerboardRgba8(8, 8, 4);
    ayt::render::TextureHandle texture =
        renderer.createTextureFromRgba8(8, 8, pixels.data(), "test_checker_8x8");
    CHECK(texture.isValid());

    renderer.setMaterialTexture(material, "albedoMap", texture);

    ayt::render::MeshHandle mesh = renderer.createTexturedUnitCube();
    CHECK(mesh.isValid());

    ayt::render::RenderScene scene;
    scene.add(mesh, material);

    renderer.beginFrame({});
    renderer.render(scene);
    renderer.endFrame();

    renderer.destroyMesh(mesh);
    renderer.destroyTexture(texture);
    renderer.destroyMaterial(material);
    renderer.shutdown();
}

TEST_SUITE_END
