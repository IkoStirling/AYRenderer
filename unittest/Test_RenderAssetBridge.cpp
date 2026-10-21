#include "AYRenderer.h"
#include "detail/RenderAssetBridge.h"
#include "detail/VertexLayoutBridge.h"
#include "detail/RenderResourceManager.h"
#include "detail/RenderPass.h"
#include "detail/TextureImageLoader.h"
#include "AYRenderer/PbrShaderSources.h"
#include "AYRenderer/MeshSubmissionInfo.h"

#include "AYResource/assetsImpl/Material.h"
#include "AYResource/assetsImpl/Mesh.h"
#include "AYResource/assetsImpl/Texture.h"

#include "AYResource/AssetPath.h"
#include "AYResource/ResourceBootstrap.h"
#include "AYResource/ResourceManager.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYIO/File.h"
#include "AYIO/Env.h"
#include "AYTest.h"

#include <bgfx/bgfx.h>

#include <filesystem>
#include <cstdio>
#include <cstdlib>
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

struct ForceScScope {
    std::optional<std::string> previous = ayt::io::env::get("AY_SHADOW_USE_SC");
    ForceScScope() { set("1"); }
    ~ForceScScope() { set(previous ? previous->c_str() : nullptr); }
    static void set(const char* value) {
#ifdef _WIN32
        _putenv_s("AY_SHADOW_USE_SC", value ? value : "");
#else
        if (value) setenv("AY_SHADOW_USE_SC", value, 1);
        else unsetenv("AY_SHADOW_USE_SC");
#endif
    }
};

} // namespace

TEST_SUITE(RenderAssetBridgeTests)

TEST_CASE(public_mesh_metadata_is_cpu_owned_and_context_identity_follows_device_lifetime)
{
    using namespace ayt::render;
    ayt::resource::Mesh source;source.createCube();std::vector<uint8_t> bytes;
    CHECK(source.saveToBinary(bytes));const auto path=tempPath("metadata_public_context.msh");
    CHECK(writeBinaryFile(path,bytes));
    auto& manager=ayt::resource::ResourceManager::instance();manager.unloadAll();
    Renderer renderer;CHECK(renderer.resourceContextId()==0);
    InitDesc init;init.backend=Backend::Noop;
    const bool ready=renderer.initialize(init);CHECK(ready);if(!ready){removeFile(path);return;}
    const auto context=renderer.resourceContextId();CHECK(context!=0);
    CHECK(renderer.initialize(init));CHECK(renderer.resourceContextId()==context);
    CHECK(!renderer.meshSubmissionInfo({}));
    CHECK_FALSE(renderer.isMaterialResident({}));
    auto material=renderer.createMaterialFromPhoskia(kBridgeMaterialShader,"metadata-resident-material");
    CHECK(material.isValid());CHECK(renderer.isMaterialResident(material));
    const auto raw=renderer.createUnitCube();CHECK(raw.isValid());CHECK(!renderer.meshSubmissionInfo(raw));
    const auto handle=renderer.loadMesh(path);CHECK(handle.isValid());
    const auto metadata=renderer.meshSubmissionInfo(handle);CHECK(metadata);
    if(metadata){CHECK(metadata->indexCount==36);CHECK(metadata->sections.size()==1);}
    Renderer moved(std::move(renderer));CHECK(renderer.resourceContextId()==0);
    CHECK(moved.resourceContextId()==context);CHECK(moved.meshSubmissionInfo(handle)==metadata);
    CHECK(moved.isMaterialResident(material));const auto oldMaterial=material;
    moved.destroyMaterial(material);CHECK_FALSE(moved.isMaterialResident(oldMaterial));
    auto destroyed=handle;moved.destroyMesh(destroyed);CHECK(!moved.meshSubmissionInfo(handle));
    if(metadata)CHECK(metadata->indexCount==36);
    moved.shutdown();CHECK(moved.resourceContextId()==0);CHECK(!moved.meshSubmissionInfo(handle));
    Renderer next;CHECK(next.initialize(init));CHECK(next.resourceContextId()!=0);
    CHECK(next.resourceContextId()!=context);next.shutdown();manager.unloadAll();removeFile(path);
}

TEST_CASE(resource_mesh_rejects_out_of_range_indices_before_uint16_narrowing)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    BGFXAdapter adapter;BGFXInitParams init;init.backend=Backend::Noop;
    const bool ready=adapter.initialize(init);CHECK(ready);if(!ready)return;
    ayt::shader::ShaderResourcePool pool;RenderResourceManager resources(adapter,pool);
    struct Cleanup {
        RenderResourceManager& resources;ayt::shader::ShaderResourcePool& pool;BGFXAdapter& adapter;
        ~Cleanup(){resources.shutdown();pool.shutdown();adapter.shutdown();}
    } cleanup{resources,pool,adapter};
    ayt::resource::Mesh source;source.createCube();
    const uint32_t indices[]={0,1,UINT32_MAX};
    source._setForTestIndices(indices,3);
    const ayt::resource::IMesh::Submesh section{0,3,0,0};
    source._setForTestSubmeshes(&section,1);
    CHECK_FALSE(uploadMeshFromResource(resources,source).isValid());
    CHECK(resources.meshes().empty());
}

TEST_CASE(cube_rejects_unrepresentable_dimensions_even_on_a_cache_hit)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    BGFXAdapter adapter;BGFXInitParams init;init.backend=Backend::Noop;
    const bool ready=adapter.initialize(init);CHECK(ready);if(!ready)return;
    ayt::shader::ShaderResourcePool pool;RenderResourceManager resources(adapter,pool);
    struct Cleanup {
        RenderResourceManager& resources;ayt::shader::ShaderResourcePool& pool;BGFXAdapter& adapter;
        ~Cleanup(){resources.shutdown();pool.shutdown();adapter.shutdown();}
    } cleanup{resources,pool,adapter};
    const std::array<uint8_t,4> pixel{9,27,81,255};
    const std::string key="cube-admission-existing-cache";
    const auto original=resources.createTextureFromRgba8(1,1,pixel.data(),key);
    CHECK(original.isValid());if(!original.isValid())return;
    // The old code returns the cached handle before narrowing/copying. This
    // safely reproduces invalid acceptance without requesting a huge buffer.
    for(uint32_t size:{65536u,UINT32_MAX,4096u}) {
        CHECK_FALSE(resources.createCubeTextureFromRgba8(size,pixel.data(),key).isValid());
        CHECK_FALSE(resources.createCubeTextureFromRgba8(size,pixel.data()).isValid());
        CHECK(resources.textures().size()==1);
        CHECK(!resources.textures().at(original.id).cube);
        CHECK(resources.createTextureFromRgba8(1,1,pixel.data(),key).id==original.id);
    }
}

TEST_CASE(target_creation_rejects_device_extent_and_bad_format_without_allocating)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    BGFXAdapter adapter;BGFXInitParams init;init.backend=Backend::Noop;
    const bool ready=adapter.initialize(init);CHECK(ready);if(!ready)return;
    struct Cleanup {BGFXAdapter& adapter;~Cleanup(){adapter.shutdown();}} cleanup{adapter};
    for(auto format:{static_cast<bgfx::TextureFormat::Enum>(-1),bgfx::TextureFormat::Count,
                     bgfx::TextureFormat::Unknown,bgfx::TextureFormat::UnknownDepth}) {
        CHECK_FALSE(bgfx::isValid(adapter.createFrameBuffer(2,2,format,false)));
        CHECK_FALSE(bgfx::isValid(adapter.createFrameBuffer(2,2,format,true)));
    }
    const auto maximum=bgfx::getCaps()->limits.maxTextureSize;
    if(maximum<UINT16_MAX) {
        const auto tooWide=static_cast<uint16_t>(maximum+1u);
        CHECK_FALSE(bgfx::isValid(adapter.createFrameBuffer(tooWide,1,bgfx::TextureFormat::RGBA8,false)));
        CHECK_FALSE(bgfx::isValid(adapter.createFrameBuffer(tooWide,1,bgfx::TextureFormat::RGBA8,true)));
        CHECK_FALSE(bgfx::isValid(adapter.createColorDepthFrameBuffer(tooWide,1)));
        CHECK_FALSE(bgfx::isValid(adapter.createGbufferFrameBuffer(tooWide,1)));
        CHECK_FALSE(bgfx::isValid(adapter.createDepthOnlyFrameBuffer(tooWide,1)));
        CHECK_FALSE(bgfx::isValid(adapter.createBlitDstTexture2D(tooWide,1)));
    }
}

TEST_CASE(raw_texture_admission_refuses_truncated_dimensions_and_invalid_format_before_allocation)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    BGFXAdapter adapter;BGFXInitParams init;init.backend=Backend::Noop;CHECK(adapter.initialize(init));
    ayt::shader::ShaderResourcePool pool;RenderResourceManager resources(adapter,pool);
    const std::array<uint8_t,4> pixel{1,2,3,255};
    CHECK_FALSE(resources.createTextureFromRgba8(65536,1,pixel.data()).isValid());
    CHECK_FALSE(resources.createTextureFromRgba8(16384,4097,pixel.data()).isValid());
    CHECK_FALSE(resources.createDynamicTextureRgba8(1,65536).isValid());
    CHECK_FALSE(resources.createTextureFromData(1,1,UINT32_MAX,pixel.data(),4).isValid());
    CHECK_FALSE(bgfx::isValid(adapter.createTexture2DFromData(1,1,
        static_cast<bgfx::TextureFormat::Enum>(-1),pixel.data(),4)));
    CHECK_FALSE(bgfx::isValid(adapter.createTexture2DFromData(1,1,
        bgfx::TextureFormat::Unknown,pixel.data(),4)));
    CHECK_FALSE(bgfx::isValid(adapter.createTexture2DFromData(1,1,
        bgfx::TextureFormat::UnknownDepth,pixel.data(),4)));
    CHECK(resources.textures().empty());
    const auto maximum=bgfx::getCaps()->limits.maxTextureSize;
    if(maximum<UINT16_MAX){
        const auto tooWide=static_cast<uint16_t>(maximum+1u);
        CHECK_FALSE(bgfx::isValid(adapter.createTexture2D(tooWide,1,pixel.data())));
        CHECK_FALSE(bgfx::isValid(adapter.createDynamicTexture2D(tooWide,1)));
        CHECK_FALSE(bgfx::isValid(adapter.createRenderTargetTexture2D(tooWide,1,bgfx::TextureFormat::RGBA8)));
    }
    resources.shutdown();pool.shutdown();adapter.shutdown();
}

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

TEST_CASE(mesh_reload_failure_retains_resident_gpu_buffers_and_retry_keeps_the_handle)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    ayt::resource::Mesh source;
    source.createCube(1);
    std::vector<ayt::resource::UInt8> bytes;
    CHECK(source.saveToBinary(bytes));
    const auto path = tempPath("reload_mesh_transaction.msh");
    CHECK(writeBinaryFile(path, bytes));
    auto& manager = ayt::resource::ResourceManager::instance();
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(false);
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    CHECK(adapter.initialize(init));
    ayt::shader::ShaderResourcePool pool;
    RenderResourceManager resources(adapter, pool);
    const auto handle = resources.loadMesh(path);
    CHECK(handle.isValid());
    auto cpu = manager.load<ayt::resource::Mesh>(path);
    CHECK(cpu);
    if (handle.isValid() && cpu) {
        const auto vertex = resources.meshes().at(handle.id).vertexBuffer;
        const auto index = resources.meshes().at(handle.id).indexBuffer;
        const auto metadata=resources.meshes().at(handle.id).submissionInfo;
        CHECK(metadata);if(metadata){CHECK(metadata->indexCount==36);CHECK(metadata->sections.size()==1);}
        // An unusable resident replacement exercises bridge admission failure;
        // this is not a claim to simulate GPU memory exhaustion.
        cpu->unload();
        CHECK_FALSE(resources.reloadMeshFromPath(path));
        CHECK(resources.meshes().contains(handle.id));
        CHECK(resources.loadMesh(path).id == handle.id);
        if (resources.meshes().contains(handle.id)) {
            CHECK(resources.meshes().at(handle.id).vertexBuffer.idx == vertex.idx);
            CHECK(resources.meshes().at(handle.id).indexBuffer.idx == index.idx);
            CHECK(resources.meshes().at(handle.id).submissionInfo==metadata);
        }
        cpu->createCube(3);
        const ayt::resource::IMesh::Submesh sections[]={{0,18,0,0},{18,18,0,0}};
        cpu->_setForTestSubmeshes(sections,2);
        CHECK(resources.reloadMeshFromPath(path));
        CHECK(resources.loadMesh(path).id == handle.id);
        CHECK(resources.meshes().contains(handle.id));
        if (resources.meshes().contains(handle.id))
            CHECK(resources.meshes().at(handle.id).vertexBuffer.idx != vertex.idx);
        const auto replacement=resources.meshes().at(handle.id).submissionInfo;
        CHECK(replacement);CHECK(replacement!=metadata);
        if(replacement){
            CHECK(replacement->sections.size()==2);
            if(replacement->sections.size()==2){
                CHECK(replacement->sections[0].indexCount==18);
                CHECK(replacement->sections[1].firstIndex==18);
            }
        }
        if(metadata)CHECK(metadata->sections.size()==1);
    }
    resources.shutdown();
    pool.shutdown();
    adapter.shutdown();
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(true);
    removeFile(path);
}

TEST_CASE(texture_reload_failure_retains_linear_and_srgb_gpu_variants_and_retry_keeps_handles)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    ayt::resource::Texture source;
    source.createSolidColor(4, 4, 255, 128, 64);
    std::vector<ayt::resource::UInt8> bytes;
    CHECK(source.saveToBinary(bytes));
    const auto path = tempPath("reload_texture_transaction.txr");
    CHECK(writeBinaryFile(path, bytes));
    auto& manager = ayt::resource::ResourceManager::instance();
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(false);
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    CHECK(adapter.initialize(init));
    ayt::shader::ShaderResourcePool pool;
    RenderResourceManager resources(adapter, pool);
    const auto linear = resources.loadTexture(path, false);
    const auto srgb = resources.loadTexture(path, true);
    CHECK(linear.isValid() && srgb.isValid());
    auto cpu = manager.load<ayt::resource::Texture>(path);
    CHECK(cpu);
    if (linear.isValid() && srgb.isValid() && cpu) {
        const auto oldLinear = resources.textures().at(linear.id).handle;
        const auto oldSrgb = resources.textures().at(srgb.id).handle;
        cpu->_width = 0;
        CHECK_FALSE(resources.reloadTextureFromPath(path));
        CHECK(resources.textures().contains(linear.id) && resources.textures().contains(srgb.id));
        CHECK(resources.loadTexture(path, false).id == linear.id);
        CHECK(resources.loadTexture(path, true).id == srgb.id);
        if (resources.textures().contains(linear.id))
            CHECK(resources.textures().at(linear.id).handle.idx == oldLinear.idx);
        if (resources.textures().contains(srgb.id))
            CHECK(resources.textures().at(srgb.id).handle.idx == oldSrgb.idx);
        cpu->createSolidColor(8, 8, 64, 128, 255);
        CHECK(resources.reloadTextureFromPath(path));
        CHECK(resources.loadTexture(path, false).id == linear.id);
        CHECK(resources.loadTexture(path, true).id == srgb.id);
        if (resources.textures().contains(linear.id))
            CHECK(resources.textures().at(linear.id).handle.idx != oldLinear.idx);
        if (resources.textures().contains(srgb.id))
            CHECK(resources.textures().at(srgb.id).handle.idx != oldSrgb.idx);
    }
    resources.shutdown();
    pool.shutdown();
    adapter.shutdown();
    manager.unloadAll();
    manager.setAutoWatchLoadedResources(true);
    removeFile(path);
}

TEST_CASE(unsigned_bc_color_expansion_preserves_channels_and_crops_partial_blocks)
{
    using ayt::render::detail::decodeUnsignedBcChannels;
    for (const bool twoChannels : {false, true}) {
        std::vector<uint8_t> blocks(twoChannels ? 32u : 16u, 0);
        const size_t blockSize = twoChannels ? 16u : 8u;
        blocks[0] = 96;
        blocks[blockSize] = 240;
        if (twoChannels) { blocks[8] = 200; blocks[blockSize + 8] = 20; }
        const auto decoded = decodeUnsignedBcChannels(5, 3, blocks.data(),
            static_cast<uint32_t>(blocks.size()), twoChannels);
        CHECK(decoded.isValid());
        CHECK(decoded.width == 5 && decoded.height == 3);
        CHECK(decoded.rgba8.size() == 60u);
        if (decoded.rgba8.size() == 60u) {
            std::fprintf(stderr, "[BC channels] two=%d first=%u,%u,%u,%u last=%u,%u,%u,%u\n",
                twoChannels ? 1 : 0, decoded.rgba8[0], decoded.rgba8[1], decoded.rgba8[2], decoded.rgba8[3],
                decoded.rgba8[16], decoded.rgba8[17], decoded.rgba8[18], decoded.rgba8[19]);
            for (size_t pixel = 0; pixel < 15u; ++pixel) {
                const bool lastColumn = pixel % 5u == 4u;
                CHECK(decoded.rgba8[pixel * 4u] == (lastColumn ? 240u : 96u));
                CHECK(decoded.rgba8[pixel * 4u + 1u] == (twoChannels ? lastColumn ? 20u : 200u : 0u));
                CHECK(decoded.rgba8[pixel * 4u + 2u] == 0u);
                CHECK(decoded.rgba8[pixel * 4u + 3u] == 255u);
            }
        }
        CHECK_FALSE(decodeUnsignedBcChannels(5, 3, blocks.data(),
            static_cast<uint32_t>(blocks.size() - 1u), twoChannels).isValid());
        CHECK_FALSE(decodeUnsignedBcChannels(5, 3, blocks.data(),
            static_cast<uint32_t>(blocks.size() + 1u), twoChannels).isValid());
        const auto single = decodeUnsignedBcChannels(1, 1, blocks.data(),
            static_cast<uint32_t>(blockSize), twoChannels);
        CHECK(single.isValid() && single.rgba8.size() == 4u && single.rgba8[0] == 96u);
    }
    uint8_t block[8]{};
    CHECK_FALSE(decodeUnsignedBcChannels(0, 4, block, 8, false).isValid());
    CHECK_FALSE(decodeUnsignedBcChannels(4, 4, nullptr, 8, false).isValid());
    CHECK_FALSE(decodeUnsignedBcChannels(65536, 4, block, 8, false).isValid());
    CHECK_FALSE(decodeUnsignedBcChannels(16384, 16384, block, 8, false).isValid());
}

TEST_CASE(bc4_and_bc5_color_uploads_use_srgb_rgba_while_data_uploads_stay_linear)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    BGFXAdapter adapter;
    BGFXInitParams init; init.backend = Backend::Noop;
    CHECK(adapter.initialize(init));
    ayt::shader::ShaderResourcePool pool;
    RenderResourceManager resources(adapter, pool);
    for (const bool twoChannels : {false, true}) {
        ayt::resource::Texture texture;
        texture._width = 4; texture._height = 4;
        texture._format = twoChannels ? ayt::resource::TextureFormat::BC5
            : ayt::resource::TextureFormat::BC4;
        std::vector<uint8_t> blocks(twoChannels ? 16u : 8u, 0);
        blocks[0] = 96; if (twoChannels) blocks[8] = 200;
        texture.setImageData(blocks.data(), blocks.size());
        texture.setMipmapLayout({0}, {static_cast<uint32_t>(blocks.size())});
        auto sampler = texture.getSampler(); sampler.sRGB = false; texture.setSampler(sampler);
        const auto linear = uploadTextureFromResource(resources, texture, {}, false);
        const auto color = uploadTextureFromResource(resources, texture, {}, true);
        CHECK(linear.isValid() && color.isValid() && linear.id != color.id);
        if (linear.isValid() && color.isValid()) {
            CHECK_FALSE(resources.textures().at(linear.id).srgb);
            CHECK(resources.textures().at(color.id).srgb);
        }
        CHECK(texture.getImageDataSize() == blocks.size());
        CHECK(texture.getImageDataBytes()[0] == 96u);
    }
    const uint8_t invalidData[16]{};
    CHECK_FALSE(bgfx::isValid(adapter.createTexture2DFromData(4, 4,
        bgfx::TextureFormat::Count, invalidData, sizeof(invalidData))));
    resources.shutdown(); pool.shutdown(); adapter.shutdown();
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

TEST_CASE(independent_material_snapshots_do_not_replace_or_mutate_path_cached_scene_materials)
{
    if (!fileExists(AY_SHADER_SHADERC_HINT))
        SKIP_TEST("shaderc is required to validate the real material bridge");
    const std::string shaderPath = tempPath("bridge_independent.phoskia");
    const std::string matPath = tempPath("bridge_independent.mat");
    CHECK(writeTextFile(shaderPath, kBridgeMaterialShader));
    ayt::resource::Material source;
    source.initialise("Scene baseline", shaderPath);
    const float identity[] = {1,1,0,0};
    source.setFloat4("uvTransform", identity);
    source.setFloat("normalStrength",1);
    source.setFloat("roughnessBias",0);
    std::vector<ayt::resource::UInt8> bytes;
    CHECK(source.saveToBinary(bytes));CHECK(writeBinaryFile(matPath,bytes));
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;desc.backend=ayt::render::Backend::Noop;
    CHECK(renderer.initialize(desc));
    auto scene=renderer.loadMaterial(matPath);
    CHECK(scene.isValid());
    const float scaled[] = {3,2,.1f,.2f};
    source.setFloat4("uvTransform",scaled);source.setFloat("normalStrength",2);
    auto first=renderer.createMaterialFromResource(source,matPath);
    auto second=renderer.createMaterialFromResource(source,matPath);
    CHECK(first.isValid());CHECK(second.isValid());
    CHECK(first.id!=scene.id);CHECK(second.id!=scene.id);CHECK(first.id!=second.id);
    CHECK(renderer.materialPbrSurfaceSettings(scene).uvScaleX==1);
    CHECK(renderer.materialPbrSurfaceSettings(first).uvScaleX==3);
    CHECK(renderer.materialPbrSurfaceSettings(second).normalStrength==2);
    renderer.setMaterialPbrSurfaceSettings(first,{7,7,0,0,0,.2f});
    CHECK(renderer.materialPbrSurfaceSettings(second).uvScaleX==3);
    CHECK(renderer.materialPbrSurfaceSettings(scene).uvScaleX==1);
    renderer.destroyMaterial(first);
    renderer.destroyMaterial(second);
    const auto reloaded=renderer.loadMaterial(matPath);
    CHECK(reloaded.id==scene.id);
    CHECK(renderer.materialPbrSurfaceSettings(scene).uvScaleX==1);
    renderer.destroyMaterial(scene);
    renderer.shutdown();
    removeFile(shaderPath);removeFile(matPath);
}

TEST_CASE(shared_material_program_survives_destroy_reload_and_final_owner_reclaims_it)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    if (!fileExists(AY_SHADER_SHADERC_HINT))
        SKIP_TEST("shaderc is required for shared GPU program lifetime checks");
    const auto shaderPath = tempPath("bridge_program_lifetime.phoskia");
    CHECK(writeTextFile(shaderPath, kBridgeMaterialShader));
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    CHECK(adapter.initialize(init));
    ayt::shader::ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT, AY_SHADER_BGFX_SRC_HINT});
    pool.resolvePlatformFromRenderer();
    RenderResourceManager resources(adapter, pool);
    auto scene = resources.createMaterialFromFile(shaderPath);
    auto preview = resources.createMaterialFromFile(shaderPath);
    auto next = resources.createMaterialFromFile(shaderPath);
    CHECK(scene.isValid() && preview.isValid() && next.isValid());
    if (scene.isValid() && preview.isValid() && next.isValid()) {
        const auto program = resources.materials().at(scene.id).shader;
        CHECK(program.isValid());
        CHECK(resources.materials().at(preview.id).shader == program);
        CHECK(resources.materials().at(next.id).shader == program);
        resources.destroyMaterial(preview);
        CHECK(program.isValid());
        // Same-revision explicit reload must not invalidate its own replacement.
        CHECK(resources.reloadMaterialsForShaderFile(shaderPath) == 2);
        CHECK(program.isValid());
        // A path-cached Scene reload keeps its stable id and a live preview.
        const auto materialPath = tempPath("bridge_program_lifetime.mat");
        CHECK(ayt::resource::initializeLoaders());
        auto& l2 = ayt::resource::ResourceManager::instance();
        const bool previousWatch = l2.autoWatchLoadedResources();
        l2.setAutoWatchLoadedResources(false);
        ayt::resource::Material source;
        source.initialise("Scene reload", shaderPath);
        source.setFloat("normalStrength", 1);
        std::vector<ayt::resource::UInt8> bytes;
        CHECK(source.saveToBinary(bytes));
        CHECK(writeBinaryFile(materialPath, bytes));
        auto cached = resources.loadMaterial(materialPath);
        CHECK(cached.isValid());
        if (cached.isValid()) {
            CHECK(resources.materials().at(cached.id).shader == program);
            source.setFloat("normalStrength", 2);
            CHECK(source.saveToBinary(bytes));
            CHECK(writeBinaryFile(materialPath, bytes));
            l2.reloadResource(materialPath);
            CHECK(resources.reloadMaterialFromPath(materialPath));
            CHECK(resources.loadMaterial(materialPath).id == cached.id);
            CHECK(resources.materialPbrSurfaceSettings(cached).normalStrength == 2);
            CHECK(program.isValid());
            source.setShader("");
            CHECK(source.saveToBinary(bytes));
            CHECK(writeBinaryFile(materialPath, bytes));
            l2.reloadResource(materialPath);
            CHECK_FALSE(resources.reloadMaterialFromPath(materialPath));
            CHECK(resources.loadMaterial(materialPath).id == cached.id);
            CHECK(resources.materialPbrSurfaceSettings(cached).normalStrength == 2);
            CHECK(resources.materials().at(cached.id).shader.isValid());
            resources.destroyMaterial(cached);
        }
        l2.unloadResource(materialPath);
        l2.setAutoWatchLoadedResources(previousWatch);
        removeFile(materialPath);
        resources.destroyMaterial(scene);
        CHECK(program.isValid());
        resources.destroyMaterial(next);
        CHECK_FALSE(program.isValid());
        auto recreated = resources.createMaterialFromFile(shaderPath);
        CHECK(recreated.isValid());
        const auto recreatedProgram = resources.materials().at(recreated.id).shader;
        CHECK(recreatedProgram.isValid());
        CHECK(recreatedProgram != program);
        resources.shutdown();
        CHECK_FALSE(recreatedProgram.isValid());
    }
    resources.shutdown();
    pool.shutdown();
    adapter.shutdown();
    removeFile(shaderPath);
}

TEST_CASE(forced_sc_bridge_keeps_material_instances_distinct_while_sharing_the_program)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    if (!fileExists(AY_SHADER_SHADERC_HINT))
        SKIP_TEST("shaderc is required for forced-SC bridge regression");
    ForceScScope forceSc;
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    CHECK(adapter.initialize(init));
    ayt::shader::ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT, AY_SHADER_BGFX_SRC_HINT});
    pool.resolvePlatformFromRenderer();
    RenderResourceManager resources(adapter, pool);
    ayt::resource::Material source;
    source.initialise("Forced SC snapshot", "simple_lit_shadow.phoskia");
    auto first = bindMaterialFromResource(resources, source, "snapshot.mat");
    auto second = bindMaterialFromResource(resources, source, "snapshot.mat");
    source.setFloat("roughnessBias", .35f);
    auto third = bindMaterialFromResource(resources, source, "snapshot.mat");
    CHECK(first.isValid() && second.isValid() && third.isValid());
    if (first.isValid() && second.isValid() && third.isValid()) {
        CHECK(first.id != second.id && second.id != third.id && first.id != third.id);
        CHECK(resources.materialCacheSize() == 0);
        const auto program = resources.materials().at(first.id).shader;
        CHECK(program.isValid());
        CHECK(program == resources.materials().at(second.id).shader);
        CHECK(program == resources.materials().at(third.id).shader);
        CHECK(resources.materialPbrSurfaceSettings(second).roughnessBias == 0);
        CHECK(resources.materialPbrSurfaceSettings(third).roughnessBias == .35f);
        resources.destroyMaterial(first);
        CHECK(program.isValid());
        resources.destroyMaterial(second);
        CHECK(program.isValid());
        resources.destroyMaterial(third);
        CHECK_FALSE(program.isValid());
    }
    resources.shutdown();
    pool.shutdown();
    adapter.shutdown();
}

TEST_CASE(sparse_standard_lit_resources_get_instance_defaults_without_overwriting_authored_factors)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    if (!fileExists(AY_SHADER_SHADERC_HINT))
        SKIP_TEST("shaderc is required for sparse resource regression");
    const auto path = tempPath("sparse_pbr_defaults.phoskia");
    CHECK(writeTextFile(path, kPbrPhoskiaSource));
    BGFXAdapter adapter;
    BGFXInitParams init;
    init.backend = Backend::Noop;
    CHECK(adapter.initialize(init));
    ayt::shader::ShaderResourcePool pool;
    pool.setShadercExecutable(AY_SHADER_SHADERC_HINT);
    pool.setBgfxIncludeDirs({AY_SHADER_BGFX_COMMON_HINT, AY_SHADER_BGFX_SRC_HINT});
    pool.resolvePlatformFromRenderer();
    RenderResourceManager resources(adapter, pool);
    ayt::resource::Material source;
    source.initialise("Sparse PBR", path);
    const auto sparse = bindMaterialFromResource(resources, source, "sparse.mat");
    CHECK(sparse.isValid());
    if (sparse.isValid()) {
        const auto& material = resources.materials().at(sparse.id);
        CHECK(materialUniformScalar(material, "metallic", -99) == 0);
        CHECK(materialUniformScalar(material, "roughness", -99) == .5f);
        CHECK(materialUniformScalar(material, "ao", -99) == 1);
        CHECK(materialUniformScalar(material, "opacity", -99) == 1);
        CHECK(materialUniformScalar(material, "opacitySource", -99) == 0);
        CHECK(materialUniformScalar(material, "normalYSign", -99) == 1);
        CHECK(materialUniformScalar(material, "premultipliedAlpha", -99) == 0);
        CHECK(materialUniformScalar(material, "emissive", -99) == 0);
        source.setFloat("metallic", .8f);
        source.setFloat("roughness", .1f);
        source.setFloat("opacity", .3f);
        const auto authored = bindMaterialFromResource(resources, source, "sparse.mat");
        CHECK(authored.isValid());
        if (authored.isValid()) {
            const auto& changed = resources.materials().at(authored.id);
            CHECK(materialUniformScalar(changed, "metallic", -99) == .8f);
            CHECK(materialUniformScalar(changed, "roughness", -99) == .1f);
            CHECK(materialUniformScalar(changed, "opacity", -99) == .3f);
            CHECK(materialUniformScalar(material, "metallic", -99) == 0);
            CHECK(materialUniformScalar(material, "roughness", -99) == .5f);
        }
    }
    resources.shutdown();
    pool.shutdown();
    adapter.shutdown();
    removeFile(path);
}


TEST_CASE(material_shader_resolves_all_mounted_roots_and_legacy_flat_fallback)
{
    if (!fileExists(AY_SHADER_SHADERC_HINT))
        SKIP_TEST("shaderc is required for shader mount resolution");
    namespace fs = std::filesystem;
    const fs::path root = tempPath("ay_bridge_shader_mounts");
    fs::create_directories(root / "project/materials");
    fs::create_directories(root / "runtime");
    const auto oldRoots = ayt::resource::assetRoots();
    struct Cleanup {
        fs::path root;
        std::vector<std::string> roots;
        ~Cleanup() {
            ayt::resource::setAssetRoots(roots);
            std::error_code error;
            fs::remove_all(root, error);
        }
    } cleanup{root, oldRoots};
    const auto shader = root / "runtime/bridge_mount.phoskia";
    CHECK(writeTextFile(shader.string(), kBridgeMaterialShader));
    ayt::resource::setAssetRoots({(root / "project").string(), (root / "runtime").string()});
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    CHECK(renderer.initialize(desc));
    for (const auto* reference : {"bridge_mount.phoskia", "shaders/bridge_mount.phoskia"}) {
        ayt::resource::Material source;
        source.initialise("Imported fallback", reference);
        auto handle = renderer.createMaterialFromResource(source,
            (root / "project/materials/imported.mat").string());
        CHECK(handle.isValid());
        renderer.destroyMaterial(handle);
    }
    renderer.shutdown();
}

TEST_CASE(texture_refresh_reports_each_variant_without_loading_unused_paths)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    using ayt::resource::ResourceRefreshState;
    auto& cpu=ayt::resource::ResourceManager::instance();
    cpu.unloadAll();cpu.setAutoWatchLoadedResources(false);
    const auto path=tempPath("reported_texture_refresh.txr");
    ayt::resource::Texture source;source.createSolidColor(4,4,255,127,63);
    std::vector<ayt::resource::UInt8> bytes;CHECK(source.saveToBinary(bytes));CHECK(writeBinaryFile(path,bytes));
    BGFXAdapter adapter;BGFXInitParams init;init.backend=Backend::Noop;CHECK(adapter.initialize(init));
    ayt::shader::ShaderResourcePool pool;RenderResourceManager resources(adapter,pool);
    auto report=resources.onResourceFileChangedWithReport(path,71);
    CHECK(report.size()==2);CHECK_FALSE(cpu.isLoaded(path));CHECK(resources.textures().empty());
    if(report.size()==2)for(const auto& entry:report){CHECK(entry.state==ResourceRefreshState::NotResident);CHECK(entry.contextId==71);CHECK(entry.resourceId==0);}
    auto linear=resources.loadTexture(path,false);auto srgb=resources.loadTexture(path,true);
    CHECK(linear.isValid());CHECK(srgb.isValid());
    auto texture=cpu.load<ayt::resource::Texture>(path);CHECK(texture);
    if(texture&&linear.isValid()&&srgb.isValid()){
        const auto oldLinear=resources.textures().at(linear.id).handle.idx;
        const auto oldSrgb=resources.textures().at(srgb.id).handle.idx;
        texture->_width=0;report=resources.onResourceFileChangedWithReport(path,71);
        CHECK(report.size()==2);
        if(report.size()==2){
            CHECK(report[0].state==ResourceRefreshState::Retained);CHECK(report[1].state==ResourceRefreshState::Retained);
            CHECK(report[0].resourceId==linear.id);CHECK(report[1].resourceId==srgb.id);
            CHECK(report[0].variant=="linear");CHECK(report[1].variant=="sRGB");
            CHECK(!report[0].diagnostic.empty());CHECK(!report[1].diagnostic.empty());
        }
        CHECK(resources.textures().at(linear.id).handle.idx==oldLinear);
        CHECK(resources.textures().at(srgb.id).handle.idx==oldSrgb);
        texture->createSolidColor(8,8,63,127,255);
        resources.destroyTexture(linear);
        report=resources.onResourceFileChangedWithReport(path,71);CHECK(report.size()==2);
        if(report.size()==2){
            CHECK(report[0].state==ResourceRefreshState::NotResident);CHECK(report[0].resourceId==0);
            CHECK(report[1].state==ResourceRefreshState::Updated);CHECK(report[1].resourceId==srgb.id);
            CHECK(report[1].contextId==71);CHECK(report[1].diagnostic.empty());
        }
        CHECK(resources.textures().at(srgb.id).handle.idx!=oldSrgb);
    }
    report=resources.onResourceFileChangedWithReport(tempPath("unsupported_refresh.blob"),71);
    CHECK(report.size()==1);if(report.size()==1)CHECK(report[0].state==ResourceRefreshState::NotHandled);
    resources.shutdown();pool.shutdown();adapter.shutdown();cpu.unloadAll();cpu.setAutoWatchLoadedResources(true);removeFile(path);
}

TEST_CASE(renderer_refresh_registration_follows_moves_and_shutdown_keeps_newer_observers)
{
    using namespace ayt::render;
    using ayt::resource::ResourceRefreshState;
    auto& cpu=ayt::resource::ResourceManager::instance();cpu.unloadAll();cpu.setAutoWatchLoadedResources(false);
    const auto path=tempPath("moved_renderer_refresh.msh");
    ayt::resource::Mesh source;source.createCube();std::vector<ayt::resource::UInt8> bytes;
    CHECK(source.saveToBinary(bytes));CHECK(writeBinaryFile(path,bytes));
    Renderer original;InitDesc init;init.backend=Backend::Noop;CHECK(original.initialize(init));
    const auto context=original.resourceContextId();const auto mesh=original.loadMesh(path);CHECK(mesh.isValid());
    const auto previous=original.meshSubmissionInfo(mesh);CHECK(previous);
    Renderer moved(std::move(original));original.shutdown();
    source.createCube(3);const ayt::resource::IMesh::Submesh sections[]={{0,18,0,0},{18,18,0,0}};
    source._setForTestSubmeshes(sections,2);CHECK(source.saveToBinary(bytes));CHECK(writeBinaryFile(path,bytes));
    auto report=cpu.refreshPublishedResourcesWithReport({path});CHECK(report.cpuRefreshed);CHECK_FALSE(report.needsAttention());
    CHECK(report.paths.size()==1);
    if(report.paths.size()==1){
        CHECK(report.paths[0].consumer=="Renderer");CHECK(report.paths[0].outcomes.size()==1);
        if(report.paths[0].outcomes.size()==1){const auto& entry=report.paths[0].outcomes[0];
            CHECK(entry.state==ResourceRefreshState::Updated);CHECK(entry.resourceId==mesh.id);CHECK(entry.contextId==context);}
    }
    CHECK(moved.meshSubmissionInfo(mesh)!=previous);
    Renderer destination;destination=std::move(moved);
    report=cpu.refreshPublishedResourcesWithReport({path});CHECK(report.cpuRefreshed);CHECK_FALSE(report.needsAttention());
    CHECK(destination.resourceContextId()==context);CHECK(moved.resourceContextId()==0);
    CHECK(report.paths.size()==1);if(report.paths.size()==1)CHECK(report.paths[0].consumer=="Renderer");
    int notifications=0;
    const auto foreign=cpu.setRefreshObserver("later observer",[&](const std::string&){
        ++notifications;return std::vector<ayt::resource::ResourceConsumerRefresh>{{ResourceRefreshState::NotResident}};
    });
    destination.shutdown();report=cpu.refreshPublishedResourcesWithReport({path});
    CHECK(report.cpuRefreshed);CHECK_FALSE(report.needsAttention());CHECK(notifications==1);
    CHECK(report.paths.size()==1);if(report.paths.size()==1)CHECK(report.paths[0].consumer=="later observer");
    CHECK(cpu.clearRefreshObserver(foreign));cpu.unloadAll();cpu.setAutoWatchLoadedResources(true);removeFile(path);
}

TEST_SUITE_END
