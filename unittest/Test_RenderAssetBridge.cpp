#include "AYRenderer.h"
#include "detail/RenderAssetBridge.h"

#include "assetsImpl/AYMesh.h"
#include "assetsImpl/AYTexture.h"

#include "AYFile.h"
#include "AYTest.h"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

struct PosVertex {
    float x;
    float y;
    float z;
};

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

void removeFile(const std::string& path)
{
    std::remove(path.c_str());
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
    CHECK(layout.elementCount == 3);
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

TEST_SUITE_END
