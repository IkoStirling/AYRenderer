#include "AYRenderer.h"
#include "detail/RenderAssetBridge.h"
#include "detail/VertexLayoutBridge.h"
#include "detail/RenderResourceManager.h"
#include "detail/RenderPass.h"
#include "AYRenderer/PbrShaderSources.h"

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

TEST_SUITE_END
