#include "AYRenderer.h"
#include "AYTest.h"

#include "assetsImpl/AYMesh.h"

#include "ayio/File.h"

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

#ifdef _WIN32
std::string tempPath(const char* name)
{
    char tempDir[MAX_PATH] = {};
    const DWORD len = GetTempPathA(MAX_PATH, tempDir);
    std::string path = name;
    if (len > 0 && len < MAX_PATH) {
        path = std::string(tempDir) + "ayengine_rd07_" + name;
    }
    return path;
}
#endif

bool writeBinaryFile(const std::string& path, const std::vector<ayt::math::UInt8>& data)
{
    ayt::io::File file(path, ayt::io::File::Mode::BinaryWrite);
    if (!file.isOpen()) {
        return false;
    }
    return file.write(data.data(), data.size()) == data.size();
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

TEST_CASE(dynamic_texture_update_rgba8)
{
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    CHECK(renderer.initialize(desc));

    ayt::render::TextureHandle dyn =
        renderer.createDynamicTextureRgba8(4, 4);
    CHECK(dyn.isValid());

    const std::vector<uint8_t> red(4 * 4 * 4, 0);
    std::vector<uint8_t> pixels = red;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 255;
        pixels[i + 3] = 255;
    }
    CHECK(renderer.updateTextureFromRgba8(dyn, pixels.data()));

    // Second update (video-style per-frame rewrite).
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 0;
        pixels[i + 1] = 255;
    }
    CHECK(renderer.updateTextureFromRgba8(dyn, pixels.data()));

    // Static createTextureFromRgba8 must reject update.
    const std::vector<uint8_t> staticPx = makeCheckerboardRgba8(4, 4, 2);
    ayt::render::TextureHandle stat =
        renderer.createTextureFromRgba8(4, 4, staticPx.data(), "static_no_update");
    CHECK(stat.isValid());
    CHECK(!renderer.updateTextureFromRgba8(stat, pixels.data()));

    CHECK(!renderer.updateTextureFromRgba8(dyn, nullptr));
    CHECK(!renderer.updateTextureFromRgba8({}, pixels.data()));

    renderer.destroyTexture(dyn);
    renderer.destroyTexture(stat);
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

// RD-07: RenderResourceManager must cache by path so the ECS layer
// (RenderSystem / SkinnedMeshRenderSystem) can call loadMesh every
// frame without re-uploading the GPU mesh. The contract is:
//   1. First loadMesh(path) populates the cache with one entry.
//   2. Subsequent loadMesh(path) for the same path returns the same
//      MeshHandle.id WITHOUT touching ResourceRegistry (proven by
//      deleting the file in between — cache hit must still succeed).
//   3. meshCacheSize() reflects the number of distinct (normalized)
//      paths, not the number of load calls.
TEST_CASE(mesh_cache_returns_same_handle_on_repeat_loads)
{
    using namespace ayt::render;

    Renderer renderer;
    InitDesc desc;
    desc.backend = Backend::Noop;
    desc.width   = 64;
    desc.height  = 64;
    CHECK(renderer.initialize(desc));

    // Bake a real .aymesh on disk (the same shape RenderResourceManager
    // will be asked to load from gameplay code).
#ifdef _WIN32
    const std::string meshPath = tempPath("rd07_cube.aymesh");
    {
        ayt::resource::Mesh mesh;
        mesh.createCube(1.0f);
        std::vector<ayt::math::UInt8> bin;
        CHECK(mesh.saveToBinary(bin));
        CHECK(writeBinaryFile(meshPath, bin));
    }

    const size_t cacheBefore = renderer.meshCacheSize();

    // First load: populates the cache.
    const ayt::render::MeshHandle first = renderer.loadMesh(meshPath);
    CHECK(first.isValid());
    const size_t cacheAfterFirst = renderer.meshCacheSize();
    CHECK(cacheAfterFirst == cacheBefore + 1u);

    // Repeat loads with the same path: cache size must not grow.
    const ayt::render::MeshHandle second = renderer.loadMesh(meshPath);
    const ayt::render::MeshHandle third  = renderer.loadMesh(meshPath);
    CHECK(renderer.meshCacheSize() == cacheAfterFirst);
    CHECK(second.id == first.id);
    CHECK(third.id  == first.id);

    // Cache hit must not depend on the file still existing on disk.
    // Delete the file, then load again — if loadMesh is doing disk I/O
    // the call will return invalid; cache hit returns the cached id.
    std::remove(meshPath.c_str());
    const ayt::render::MeshHandle afterDelete = renderer.loadMesh(meshPath);
    CHECK(afterDelete.id == first.id);

    // Distinct path: write a SECOND .aymesh (same content, different
    // name) and confirm the cache grows by one AND the new handle
    // differs from the first. The second file must use the .aymesh
    // extension — ResourceRegistry only recognizes known suffixes,
    // and an invalid handle would skip the cache write at L336.
    const std::string altPath = tempPath("rd07_cube2.aymesh");
    {
        ayt::resource::Mesh mesh;
        mesh.createCube(1.0f);
        std::vector<ayt::math::UInt8> bin;
        CHECK(mesh.saveToBinary(bin));
        CHECK(writeBinaryFile(altPath, bin));
    }
    const ayt::render::MeshHandle alt = renderer.loadMesh(altPath);
    CHECK(alt.isValid());
    CHECK(alt.id != first.id);
    CHECK(renderer.meshCacheSize() == cacheAfterFirst + 1u);
    std::remove(altPath.c_str());
#else
    printf("    [SKIP] RD-07 test is Win32-only (uses Windows temp paths)\n");
#endif

    renderer.shutdown();
}

TEST_SUITE_END
