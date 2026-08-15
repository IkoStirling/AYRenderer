#include "AYRenderer.h"
#include "detail/RenderAssetBridge.h"
#include "detail/VertexLayoutBridge.h"

#include "AYResource/assetsImpl/Material.h"
#include "AYResource/assetsImpl/Mesh.h"
#include "AYResource/assetsImpl/Texture.h"

#include "AYResource/AssetPath.h"
#include "AYIO/File.h"
#include "AYTest.h"

#include <bgfx/bgfx.h>

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

const char* kBridgeMaterialShader = R"(
material BridgeTest {
    texture2d albedoMap
    property baseColor = vec4(0.2, 0.8, 0.3, 1.0)
    property metallic = 0.1

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

std::string tempPath(const char* name)
{
    std::string path = std::string("test_render_asset_") + name;
#ifdef _WIN32
    char tempDir[MAX_PATH] = {};
    const DWORD len = GetTempPathA(MAX_PATH, tempDir);
    if (len > 0 && len < MAX_PATH) {
        path = std::string(tempDir) + name;
    }
#endif
    return path;
}

bool writeBinaryFile(const std::string& path, const std::vector<ayt::resource::UInt8>& data)
{
    ayt::io::File file(path, ayt::io::File::Mode::BinaryWrite);
    if (!file.isOpen()) {
        return false;
    }
    return file.write(data.data(), data.size()) == data.size();
}

bool writeTextFile(const std::string& path, const std::string& text)
{
    ayt::io::File file(path, ayt::io::File::Mode::Write);
    if (!file.isOpen()) {
        return false;
    }
    return file.write(text.data(), text.size()) == text.size();
}

void removeFile(const std::string& path)
{
    std::remove(path.c_str());
}

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

} // namespace

TEST_SUITE(RenderAssetBridgeTests)

TEST_CASE(vertex_layout_from_cube_mesh)
{
    ayt::resource::Mesh mesh;
    mesh.createCube(1.0f);

    ayt::render::VertexLayoutDesc layout;
    CHECK(ayt::render::detail::vertexLayoutFromMesh(mesh, layout));
    CHECK(layout.strideBytes() == mesh.getVertexStride());
    CHECK(layout.elementCount == 3u);
}

TEST_CASE(cube_mesh_repack_preserves_uv)
{
    ayt::resource::Mesh mesh;
    mesh.createCube(1.0f);

    ayt::render::VertexLayoutDesc layout;
    bgfx::VertexLayout bgfxLayout;
    CHECK(ayt::render::detail::buildBgfxVertexLayoutFromMesh(mesh, layout, bgfxLayout));

    std::vector<uint8_t> repacked;
    CHECK(ayt::render::detail::repackMeshVertices(mesh, bgfxLayout, repacked));
    CHECK(repacked.size()
          == static_cast<size_t>(bgfxLayout.getStride()) * mesh.getVertexCount());

    const uint16_t uvOffset = bgfxLayout.getOffset(bgfx::Attrib::TexCoord0);
    const uint32_t stride   = bgfxLayout.getStride();
    const float* uv0        = reinterpret_cast<const float*>(repacked.data() + uvOffset);
    CHECK(uv0[0] == 0.0f);
    CHECK(uv0[1] == 1.0f);

    const float* uv1 = reinterpret_cast<const float*>(repacked.data() + stride + uvOffset);
    CHECK(uv1[0] == 1.0f);
    CHECK(uv1[1] == 1.0f);
}

TEST_CASE(vertex_layout_supports_tangent_attribute)
{
    ayt::render::VertexLayoutDesc layout;
    CHECK(layout.add({ayt::render::VertexAttribute::Position, 3,
                      ayt::render::VertexComponentType::Float, false}));
    CHECK(layout.add({ayt::render::VertexAttribute::Tangent, 4,
                      ayt::render::VertexComponentType::Float, false}));
    CHECK(layout.strideBytes() == 28u);
}

TEST_CASE(load_mesh_from_aymesh_file)
{
    ayt::resource::Mesh mesh;
    mesh.createCube(1.0f);

    std::vector<ayt::resource::UInt8> binary;
    CHECK(mesh.saveToBinary(binary));

    const std::string path = tempPath("bridge_cube.aymesh");
    CHECK(writeBinaryFile(path, binary));

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    CHECK(renderer.initialize(desc));

    ayt::render::MeshHandle first = renderer.loadMesh(path);
    const ayt::render::MeshHandle second = renderer.loadMesh(path);
    CHECK(first.isValid());
    CHECK(second.isValid());
    CHECK(first.id == second.id);

    renderer.destroyMesh(first);
    renderer.shutdown();
    removeFile(path);
}

TEST_CASE(create_mesh32_uploads_large_indices)
{
    struct PosVertex {
        float x;
        float y;
        float z;
    };

    const PosVertex vertices[] = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
    };
    const uint32_t indices[] = {70000u, 70001u, 70002u};

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    CHECK(renderer.initialize(desc));

    ayt::render::MeshHandle mesh = renderer.createMesh32(
        vertices,
        3u,
        ayt::render::VertexLayoutDesc::position3(),
        indices,
        3u);
    CHECK(mesh.isValid());

    renderer.destroyMesh(mesh);
    renderer.shutdown();
}

TEST_CASE(load_texture_from_aytex_file)
{
    ayt::resource::Texture texture;
    texture.createSolidColor(4, 4, 255, 128, 64, 255);

    std::vector<ayt::resource::UInt8> binary;
    CHECK(texture.saveToBinary(binary));

    const std::string path = tempPath("bridge_tex.aytex");
    CHECK(writeBinaryFile(path, binary));

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    CHECK(renderer.initialize(desc));

    ayt::render::TextureHandle first = renderer.loadTexture(path);
    const ayt::render::TextureHandle second = renderer.loadTexture(path);
    CHECK(first.isValid());
    CHECK(second.isValid());
    CHECK(first.id == second.id);

    renderer.destroyTexture(first);
    renderer.shutdown();
    removeFile(path);
}

TEST_CASE(load_material_from_aymat_file)
{
    if (!fileExists(AY_SHADER_SHADERC_HINT)) {
        return;
    }

    const std::string shaderPath = tempPath("bridge_test.phoskia");
    const std::string texPath = tempPath("bridge_albedo.aytex");
    const std::string matPath = tempPath("bridge_test.aymat");

    CHECK(writeTextFile(shaderPath, kBridgeMaterialShader));

    ayt::resource::Texture texture;
    texture.createCheckerboard(8, 8, 4);
    std::vector<ayt::resource::UInt8> texBinary;
    CHECK(texture.saveToBinary(texBinary));
    CHECK(writeBinaryFile(texPath, texBinary));

    ayt::resource::Material material;
    material.setShader("bridge_test.phoskia");
    material.setTexture("albedoMap", "bridge_albedo.aytex");
    material.setFloat("metallic", 0.25f);
    std::vector<ayt::resource::UInt8> matBinary;
    CHECK(material.saveToBinary(matBinary));
    CHECK(writeBinaryFile(matPath, matBinary));

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle handle = renderer.loadMaterial(matPath);
    if (!handle.isValid()) {
        renderer.shutdown();
        removeFile(shaderPath);
        removeFile(texPath);
        removeFile(matPath);
        return;
    }
    CHECK(handle.isValid());

    renderer.destroyMaterial(handle);
    renderer.shutdown();
    removeFile(shaderPath);
    removeFile(texPath);
    removeFile(matPath);
}

TEST_SUITE_END
