#include "detail/RenderResourceManager.h"

#include "detail/RenderAssetBridge.h"
#include "detail/TextureImageLoader.h"
#include "detail/VertexLayoutBridge.h"

#include "AYAssetPath.h"
#include "AYResourceBootstrap.h"
#include "AYResourceRegistry.h"
#include "IAYResource.h"
#include "IAYMaterial.h"
#include "IAYMesh.h"
#include "IAYTexture.h"

#include <AYFile.h>

#include <bgfx/bgfx.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ayt::render::detail
{

namespace {

void storeUniformSlot(GpuMaterial& material, const char* name, shader::BindingId binding,
                      const void* data, size_t size)
{
    if (name == nullptr || binding == shader::InvalidBinding || data == nullptr
        || size == 0 || size > 64) {
        return;
    }

    for (GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name == name) {
            slot.binding = binding;
            std::memcpy(slot.data, data, size);
            slot.size = static_cast<uint16_t>(size);
            return;
        }
    }

    GpuMaterial::UniformSlot slot;
    slot.name    = name;
    slot.binding = binding;
    std::memcpy(slot.data, data, size);
    slot.size = static_cast<uint16_t>(size);
    material.uniformSlots.push_back(slot);
}

struct PosVertex {
    float x;
    float y;
    float z;
};

struct PosUvVertex {
    float x;
    float y;
    float z;
    float u;
    float v;
};

const PosVertex kUnitCubeVertices[] = {
    {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f}, {1.0f,  1.0f, -1.0f}, {-1.0f,  1.0f, -1.0f},
    {-1.0f, -1.0f,  1.0f}, {1.0f, -1.0f,  1.0f}, {1.0f,  1.0f,  1.0f}, {-1.0f,  1.0f,  1.0f},
};

const uint16_t kUnitCubeIndices[] = {
    4, 5, 6,  4, 6, 7,
    0, 2, 1,  0, 3, 2,
    1, 2, 6,  1, 6, 5,
    0, 4, 7,  0, 7, 3,
    3, 6, 2,  3, 7, 6,
    0, 5, 1,  0, 4, 5,
};

const PosUvVertex kTexturedCubeVertices[] = {
    // -Z
    {-1.0f, -1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, -1.0f, -1.0f, 1.0f, 1.0f},
    {1.0f,  1.0f, -1.0f, 1.0f, 0.0f}, {-1.0f,  1.0f, -1.0f, 0.0f, 0.0f},
    // +Z
    {-1.0f, -1.0f,  1.0f, 0.0f, 1.0f}, {1.0f, -1.0f,  1.0f, 1.0f, 1.0f},
    {1.0f,  1.0f,  1.0f, 1.0f, 0.0f}, {-1.0f,  1.0f,  1.0f, 0.0f, 0.0f},
    // +X
    {1.0f, -1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, -1.0f,  1.0f, 1.0f, 1.0f},
    {1.0f,  1.0f,  1.0f, 1.0f, 0.0f}, {1.0f,  1.0f, -1.0f, 0.0f, 0.0f},
    // -X
    {-1.0f, -1.0f,  1.0f, 0.0f, 1.0f}, {-1.0f, -1.0f, -1.0f, 1.0f, 1.0f},
    {-1.0f,  1.0f, -1.0f, 1.0f, 0.0f}, {-1.0f,  1.0f,  1.0f, 0.0f, 0.0f},
    // +Y
    {-1.0f,  1.0f, -1.0f, 0.0f, 1.0f}, {1.0f,  1.0f, -1.0f, 1.0f, 1.0f},
    {1.0f,  1.0f,  1.0f, 1.0f, 0.0f}, {-1.0f,  1.0f,  1.0f, 0.0f, 0.0f},
    // -Y
    {-1.0f, -1.0f,  1.0f, 0.0f, 1.0f}, {1.0f, -1.0f,  1.0f, 1.0f, 1.0f},
    {1.0f, -1.0f, -1.0f, 1.0f, 0.0f}, {-1.0f, -1.0f, -1.0f, 0.0f, 0.0f},
};

const uint16_t kTexturedCubeIndices[] = {
    0, 2, 1,  0, 3, 2,
    4, 5, 6,  4, 6, 7,
    8, 10, 9,  8, 11, 10,
    12, 13, 14,  12, 14, 15,
    16, 18, 17,  16, 19, 18,
    20, 21, 22,  20, 22, 23,
};

} // namespace

RenderResourceManager::RenderResourceManager(BGFXAdapter& adapter,
                                             shader::ShaderResourcePool& shaderPool)
    : _adapter(adapter)
    , _shaderPool(shaderPool)
{
}

void RenderResourceManager::shutdown()
{
    releaseAllMaterials();
    destroyAllMeshes();
    destroyAllTextures();
    _materialCacheByKey.clear();
    _textureCacheByKey.clear();
    _meshCacheByKey.clear();
}

void RenderResourceManager::releaseAllMaterials()
{
    for (auto& [id, mat] : _materials) {
        (void)id;
        if (mat.shader.isValid()) {
            _shaderPool.release(mat.shader);
        }
    }
    _materials.clear();
    _materialCacheByKey.clear();
}

void RenderResourceManager::destroyAllMeshes()
{
    for (auto& [id, mesh] : _meshes) {
        (void)id;
        _adapter.destroy(mesh.vertexBuffer);
        _adapter.destroy(mesh.indexBuffer);
    }
    _meshes.clear();
}

void RenderResourceManager::destroyAllTextures()
{
    for (auto& [id, tex] : _textures) {
        (void)id;
        _adapter.destroy(tex.handle);
    }
    _textures.clear();
    _textureCacheByKey.clear();
}

void RenderResourceManager::removeMaterialCacheEntry(uint64_t id)
{
    for (auto it = _materialCacheByKey.begin(); it != _materialCacheByKey.end();) {
        if (it->second == id) {
            it = _materialCacheByKey.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderResourceManager::removeTextureCacheEntry(uint64_t id)
{
    for (auto it = _textureCacheByKey.begin(); it != _textureCacheByKey.end();) {
        if (it->second == id) {
            it = _textureCacheByKey.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderResourceManager::removeMeshCacheEntry(uint64_t id)
{
    for (auto it = _meshCacheByKey.begin(); it != _meshCacheByKey.end();) {
        if (it->second == id) {
            it = _meshCacheByKey.erase(it);
        } else {
            ++it;
        }
    }
}

MeshHandle RenderResourceManager::uploadMeshInternal(const void* vertices,
                                                     uint32_t vertexCount,
                                                     uint32_t vertexStride,
                                                     const VertexLayoutDesc& layout,
                                                     const void* indices,
                                                     uint32_t indexCount,
                                                     bool use32BitIndices,
                                                     bool hasSkinWeights)
{
    MeshHandle out;
    if (!_adapter.isInitialized() || vertices == nullptr || indices == nullptr
        || vertexCount == 0 || indexCount == 0 || vertexStride == 0 || !layout.isValid()) {
        return out;
    }

    bgfx::VertexLayout bgfxLayout;
    if (!buildBgfxVertexLayout(layout, bgfxLayout)) {
        std::fprintf(stderr,
                     "[RenderResourceManager] buildBgfxVertexLayout failed (decl=%u bgfx=%u)\n",
                     layout.strideBytes(), bgfxLayout.getStride());
        return out;
    }

    const uint32_t uploadStride = bgfxLayout.getStride() > 0 ? bgfxLayout.getStride() : vertexStride;
    const uint32_t vertexBytes  = uploadStride * vertexCount;
    const uint32_t indexBytes   = use32BitIndices
        ? indexCount * sizeof(uint32_t)
        : indexCount * sizeof(uint16_t);
    const uint16_t indexFlags   = use32BitIndices ? BGFX_BUFFER_INDEX32 : BGFX_BUFFER_NONE;

    GpuMesh mesh;
    mesh.layout = layout;
    mesh.vertexCount     = vertexCount;
    mesh.indexCount      = indexCount;
    mesh.hasSkinWeights  = hasSkinWeights;
    mesh.vertexBuffer = _adapter.createVertexBuffer(vertices, vertexBytes, bgfxLayout);
    mesh.indexBuffer  = _adapter.createIndexBuffer(indices, indexBytes, indexFlags);

    if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(mesh.indexBuffer)) {
        std::fprintf(stderr,
                     "[RenderResourceManager] GPU mesh upload failed (stride=%u verts=%u indices=%u)\n",
                     uploadStride, vertexCount, indexCount);
        _adapter.destroy(mesh.vertexBuffer);
        _adapter.destroy(mesh.indexBuffer);
        return out;
    }

    const uint64_t id = _nextMeshId++;
    _meshes.emplace(id, mesh);
    out.id = id;
    return out;
}

MeshHandle RenderResourceManager::createMesh(const void* vertices,
                                             uint32_t vertexCount,
                                             const VertexLayoutDesc& layout,
                                             const uint16_t* indices,
                                             uint32_t indexCount)
{
    return uploadMeshInternal(vertices, vertexCount, layout.strideBytes(), layout, indices,
                              indexCount, false);
}

MeshHandle RenderResourceManager::createMesh32(const void* vertices,
                                               uint32_t vertexCount,
                                               const VertexLayoutDesc& layout,
                                               const uint32_t* indices,
                                               uint32_t indexCount)
{
    return uploadMeshInternal(vertices, vertexCount, layout.strideBytes(), layout, indices,
                              indexCount, true);
}

MeshHandle RenderResourceManager::createMeshFromResourceData(const void* vertices,
                                                             uint32_t vertexCount,
                                                             uint32_t vertexStride,
                                                             const VertexLayoutDesc& layout,
                                                             const uint32_t* indices,
                                                             uint32_t indexCount,
                                                             bool hasSkinWeights)
{
    if (indices == nullptr || indexCount == 0) {
        return {};
    }

    uint32_t maxIndex = 0;
    for (uint32_t i = 0; i < indexCount; ++i) {
        maxIndex = std::max(maxIndex, indices[i]);
    }

    if (maxIndex < 65536u) {
        std::vector<uint16_t> narrowed(indexCount);
        for (uint32_t i = 0; i < indexCount; ++i) {
            narrowed[i] = static_cast<uint16_t>(indices[i]);
        }
        return uploadMeshInternal(vertices, vertexCount, vertexStride, layout, narrowed.data(),
                                  indexCount, false, hasSkinWeights);
    }

    return uploadMeshInternal(vertices, vertexCount, vertexStride, layout, indices, indexCount,
                              true, hasSkinWeights);
}

MeshHandle RenderResourceManager::loadMesh(const std::string& path)
{
    if (path.empty()) {
        return {};
    }

    const std::string key = normalizeAssetPathKey(path);
    if (!key.empty()) {
        const auto cached = _meshCacheByKey.find(key);
        if (cached != _meshCacheByKey.end()) {
            MeshHandle out;
            out.id = cached->second;
            return out;
        }
    }

    ayt::resource::initializeLoaders();
    const std::shared_ptr<ayt::resource::IResource> resource =
        ayt::resource::ResourceRegistry::loadByPath(path);
    if (!resource) {
        std::fprintf(stderr, "[RenderResourceManager] loadMesh failed: ResourceRegistry '%s'\n",
                     path.c_str());
        return {};
    }

    const auto* mesh = dynamic_cast<const ayt::resource::IMesh*>(resource.get());
    if (mesh == nullptr) {
        std::fprintf(stderr, "[RenderResourceManager] loadMesh failed: not IMesh '%s'\n",
                     path.c_str());
        return {};
    }

    const MeshHandle handle = uploadMeshFromResource(*this, *mesh);
    if (!handle.isValid()) {
        std::fprintf(stderr,
                     "[RenderResourceManager] loadMesh failed: GPU upload '%s' (verts=%u stride=%u)\n",
                     path.c_str(), mesh->getVertexCount(), mesh->getVertexStride());
    }
    if (handle.isValid() && !key.empty()) {
        _meshCacheByKey.emplace(key, handle.id);
    }
    return handle;
}

MeshHandle RenderResourceManager::createUnitCube()
{
    if (!_adapter.isInitialized()) {
        return {};
    }
    return createMesh(kUnitCubeVertices,
                      static_cast<uint32_t>(sizeof(kUnitCubeVertices) / sizeof(PosVertex)),
                      VertexLayoutDesc::position3(),
                      kUnitCubeIndices,
                      static_cast<uint32_t>(sizeof(kUnitCubeIndices) / sizeof(uint16_t)));
}

MeshHandle RenderResourceManager::createTexturedUnitCube()
{
    if (!_adapter.isInitialized()) {
        return {};
    }
    return createMesh(kTexturedCubeVertices,
                      static_cast<uint32_t>(sizeof(kTexturedCubeVertices) / sizeof(PosUvVertex)),
                      VertexLayoutDesc::position3TexCoord2(),
                      kTexturedCubeIndices,
                      static_cast<uint32_t>(sizeof(kTexturedCubeIndices) / sizeof(uint16_t)));
}

void RenderResourceManager::destroyMesh(MeshHandle& mesh)
{
    if (!mesh.isValid()) {
        mesh = {};
        return;
    }

    removeMeshCacheEntry(mesh.id);
    _meshCacheByKey.clear();  // Phase 1: invalidate on destroy; full LRU is future work.

    const auto it = _meshes.find(mesh.id);
    if (it != _meshes.end()) {
        _adapter.destroy(it->second.vertexBuffer);
        _adapter.destroy(it->second.indexBuffer);
        _meshes.erase(it);
    }
    mesh = {};
}

MaterialHandle RenderResourceManager::createMaterialFromPhoskia(const std::string& source,
                                                                const std::string& cacheKey)
{
    MaterialHandle out;
    if (source.empty()) {
        return out;
    }

    if (!cacheKey.empty()) {
        const auto cached = _materialCacheByKey.find(cacheKey);
        if (cached != _materialCacheByKey.end()) {
            out.id = cached->second;
            return out;
        }
    }

    shader::ShaderResource shader = _shaderPool.acquire(source, cacheKey);
    if (!shader.isValid()) {
        for (const std::string& err : _shaderPool.lastCompileErrors()) {
            std::fprintf(stderr, "  shader: %s\n", err.c_str());
        }
        return out;
    }

    GpuMaterial material;
    material.shader = shader;

    const uint64_t id = _nextMaterialId++;
    _materials.emplace(id, std::move(material));
    if (!cacheKey.empty()) {
        _materialCacheByKey.emplace(cacheKey, id);
    }
    out.id = id;
    return out;
}

void RenderResourceManager::resetMaterialBindingCache(GpuMaterial& material)
{
    material.colorBinding = shader::InvalidBinding;
    material.mat4Binding  = shader::InvalidBinding;
}

void RenderResourceManager::rebindMaterialAfterShaderSwap(GpuMaterial& material)
{
    resetMaterialBindingCache(material);

    for (GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name.empty()) {
            slot.binding = shader::InvalidBinding;
            continue;
        }
        slot.binding = material.shader.getUniformBinding(slot.name);
    }
    for (GpuMaterial::TextureSlot& slot : material.textures) {
        if (slot.name.empty()) {
            slot.binding = shader::InvalidBinding;
            continue;
        }
        slot.binding = material.shader.getTextureBinding(slot.name);
    }
}

MaterialHandle RenderResourceManager::createMaterialFromFile(const std::string& path)
{
    if (path.empty()) {
        std::fprintf(stderr, "[RenderResourceManager] createMaterialFromFile: empty path\n");
        return MaterialHandle{};
    }
    if (!ayt::io::File::exists(path)) {
        std::fprintf(stderr, "[RenderResourceManager] createMaterialFromFile: missing '%s'\n",
                     path.c_str());
        return MaterialHandle{};
    }

    const std::string key = normalizeAssetPathKey(path);
    if (!key.empty()) {
        const auto cached = _materialCacheByKey.find(key);
        if (cached != _materialCacheByKey.end()) {
            MaterialHandle out;
            out.id = cached->second;
            return out;
        }
    }

    // compileFromFile registers a hot-reload watch on `path` (acquire(source) does not).
    shader::ShaderResource shader = _shaderPool.compileFromFile(path);
    if (!shader.isValid()) {
        for (const std::string& err : _shaderPool.lastCompileErrors()) {
            std::fprintf(stderr, "  shader: %s\n", err.c_str());
        }
        return MaterialHandle{};
    }

    GpuMaterial material;
    material.shader           = shader;
    material.shaderSourcePath = path;

    const uint64_t id = _nextMaterialId++;
    _materials.emplace(id, std::move(material));
    if (!key.empty()) {
        _materialCacheByKey.emplace(key, id);
    }

    MaterialHandle out;
    out.id = id;
    return out;
}

void RenderResourceManager::refreshMaterialsAfterHotReload()
{
    for (auto& [materialId, material] : _materials) {
        (void)materialId;
        if (material.shaderSourcePath.empty() || material.shader.isValid()) {
            continue;
        }

        std::fprintf(stderr,
                     "[RenderResourceManager] hot-reload refresh '%s'\n",
                     material.shaderSourcePath.c_str());

        shader::ShaderResource fresh =
            _shaderPool.compileFromFile(material.shaderSourcePath);
        if (!fresh.isValid()) {
            for (const std::string& err : _shaderPool.lastCompileErrors()) {
                std::fprintf(stderr, "  shader reload: %s\n", err.c_str());
            }
            continue;
        }

        material.shader = fresh;
        rebindMaterialAfterShaderSwap(material);
    }
}

MaterialHandle RenderResourceManager::loadMaterial(const std::string& path)
{
    if (path.empty()) {
        return {};
    }

    const std::string key = normalizeAssetPathKey(path);
    if (!key.empty()) {
        const auto cached = _materialCacheByKey.find(key);
        if (cached != _materialCacheByKey.end()) {
            MaterialHandle out;
            out.id = cached->second;
            return out;
        }
    }

    ayt::resource::initializeLoaders();
    const std::shared_ptr<ayt::resource::IResource> resource =
        ayt::resource::ResourceRegistry::loadByPath(path);
    if (!resource) {
        std::fprintf(stderr, "[RenderResourceManager] loadMaterial failed: ResourceRegistry '%s'\n",
                     path.c_str());
        return {};
    }

    const auto* material = dynamic_cast<const ayt::resource::IMaterial*>(resource.get());
    if (material == nullptr) {
        std::fprintf(stderr, "[RenderResourceManager] loadMaterial failed: not IMaterial '%s'\n",
                     path.c_str());
        return {};
    }

    MaterialHandle handle = bindMaterialFromResource(*this, *material, path);
    if (!handle.isValid()) {
        std::fprintf(stderr, "[RenderResourceManager] loadMaterial failed: bridge '%s'\n",
                     path.c_str());
    }
    if (handle.isValid() && !key.empty()) {
        _materialCacheByKey.emplace(key, handle.id);
    }
    return handle;
}

void RenderResourceManager::destroyMaterial(MaterialHandle& material)
{
    if (!material.isValid()) {
        material = {};
        return;
    }

    removeMaterialCacheEntry(material.id);

    const auto it = _materials.find(material.id);
    if (it != _materials.end()) {
        if (it->second.shader.isValid()) {
            _shaderPool.release(it->second.shader);
        }
        _materials.erase(it);
    }
    material = {};
}

void RenderResourceManager::setMaterialColor(MaterialHandle material, const char* propertyName,
                                             float r, float g, float b, float a)
{
    if (!material.isValid() || propertyName == nullptr) {
        return;
    }

    const auto it = _materials.find(material.id);
    if (it == _materials.end()) {
        return;
    }

    GpuMaterial& mat = it->second;
    mat.colorOverride    = ayt::math::FVector4(r, g, b, a);
    mat.hasColorOverride = true;

    shader::BindingId binding = mat.shader.getUniformBinding(propertyName);
    if (binding == shader::InvalidBinding) {
        binding = mat.shader.getUniformBinding("baseColor");
    }
    mat.colorBinding = binding;
}

void RenderResourceManager::setMaterialFloat(MaterialHandle material, const char* uniformName,
                                             float value)
{
    if (!material.isValid() || uniformName == nullptr) {
        return;
    }

    const auto it = _materials.find(material.id);
    if (it == _materials.end()) {
        return;
    }

    GpuMaterial& mat = it->second;
    const shader::BindingId binding = mat.shader.getUniformBinding(uniformName);
    storeUniformSlot(mat, uniformName, binding, &value, sizeof(float));
}

void RenderResourceManager::setMaterialVec2(MaterialHandle material, const char* uniformName,
                                            float x, float y)
{
    if (!material.isValid() || uniformName == nullptr) {
        return;
    }

    const auto it = _materials.find(material.id);
    if (it == _materials.end()) {
        return;
    }

    const float values[2] = {x, y};
    GpuMaterial& mat = it->second;
    const shader::BindingId binding = mat.shader.getUniformBinding(uniformName);
    storeUniformSlot(mat, uniformName, binding, values, sizeof(values));
}

void RenderResourceManager::setMaterialVec3(MaterialHandle material, const char* uniformName,
                                            float x, float y, float z)
{
    if (!material.isValid() || uniformName == nullptr) {
        return;
    }

    const auto it = _materials.find(material.id);
    if (it == _materials.end()) {
        return;
    }

    const float values[3] = {x, y, z};
    GpuMaterial& mat = it->second;
    const shader::BindingId binding = mat.shader.getUniformBinding(uniformName);
    const float padded[4] = {values[0], values[1], values[2], 0.0f};
    storeUniformSlot(mat, uniformName, binding, padded, sizeof(padded));
}

void RenderResourceManager::setMaterialMatrix4(MaterialHandle material, const char* uniformName,
                                               const ayt::math::Float4x4& matrix)
{
    if (!material.isValid() || uniformName == nullptr) {
        return;
    }

    const auto it = _materials.find(material.id);
    if (it == _materials.end()) {
        return;
    }

    GpuMaterial& mat = it->second;
    mat.mat4Override    = matrix;
    mat.hasMat4Override = true;
    mat.mat4Binding     = mat.shader.getUniformBinding(uniformName);
}

void RenderResourceManager::setMaterialTexture(MaterialHandle material,
                                               const char* textureBindingName,
                                               TextureHandle texture)
{
    if (!material.isValid() || textureBindingName == nullptr || !texture.isValid()) {
        return;
    }

    const auto matIt = _materials.find(material.id);
    const auto texIt = _textures.find(texture.id);
    if (matIt == _materials.end() || texIt == _textures.end()) {
        return;
    }

    GpuMaterial& mat = matIt->second;
    const shader::BindingId binding = mat.shader.getTextureBinding(textureBindingName);
    if (binding == shader::InvalidBinding) {
        std::fprintf(stderr,
                     "[RenderResourceManager] setMaterialTexture: no binding '%s'\n",
                     textureBindingName);
        return;
    }

    for (GpuMaterial::TextureSlot& slot : mat.textures) {
        if (slot.name == textureBindingName) {
            slot.binding = binding;
            slot.texture   = texture;
            return;
        }
    }

    GpuMaterial::TextureSlot slot;
    slot.name    = textureBindingName;
    slot.binding = binding;
    slot.texture = texture;
    mat.textures.push_back(slot);
}

TextureHandle RenderResourceManager::createTextureFromRgba8(uint32_t width, uint32_t height,
                                                            const uint8_t* pixels,
                                                            const std::string& cacheKey)
{
    TextureHandle out;
    if (!_adapter.isInitialized() || width == 0 || height == 0 || pixels == nullptr) {
        return out;
    }

    if (!cacheKey.empty()) {
        const auto cached = _textureCacheByKey.find(cacheKey);
        if (cached != _textureCacheByKey.end()) {
            out.id = cached->second;
            return out;
        }
    }

    GpuTexture gpuTex;
    gpuTex.width  = static_cast<uint16_t>(width);
    gpuTex.height = static_cast<uint16_t>(height);
    gpuTex.handle = _adapter.createTexture2D(gpuTex.width, gpuTex.height, pixels);
    if (!bgfx::isValid(gpuTex.handle)) {
        return out;
    }

    const uint64_t id = _nextTextureId++;
    _textures.emplace(id, gpuTex);
    if (!cacheKey.empty()) {
        _textureCacheByKey.emplace(cacheKey, id);
    }
    out.id = id;
    return out;
}

TextureHandle RenderResourceManager::createTextureFromData(uint32_t width, uint32_t height,
                                                           uint32_t bgfxTextureFormat,
                                                           const void* data, uint32_t size,
                                                           const std::string& cacheKey)
{
    TextureHandle out;
    if (!_adapter.isInitialized() || width == 0 || height == 0 || data == nullptr || size == 0) {
        return out;
    }

    if (!cacheKey.empty()) {
        const auto cached = _textureCacheByKey.find(cacheKey);
        if (cached != _textureCacheByKey.end()) {
            out.id = cached->second;
            return out;
        }
    }

    GpuTexture gpuTex;
    gpuTex.width  = static_cast<uint16_t>(width);
    gpuTex.height = static_cast<uint16_t>(height);
    gpuTex.handle = _adapter.createTexture2DFromData(
        gpuTex.width, gpuTex.height,
        static_cast<bgfx::TextureFormat::Enum>(bgfxTextureFormat),
        data, size);
    if (!bgfx::isValid(gpuTex.handle)) {
        return out;
    }

    const uint64_t id = _nextTextureId++;
    _textures.emplace(id, gpuTex);
    if (!cacheKey.empty()) {
        _textureCacheByKey.emplace(cacheKey, id);
    }
    out.id = id;
    return out;
}

TextureHandle RenderResourceManager::createTextureFromFile(const std::string& path,
                                                           const std::string& cacheKey)
{
    const std::string key = !cacheKey.empty() ? cacheKey : normalizeAssetPathKey(path);
    if (!key.empty()) {
        const auto cached = _textureCacheByKey.find(key);
        if (cached != _textureCacheByKey.end()) {
            TextureHandle out;
            out.id = cached->second;
            return out;
        }
    }

    const DecodedImage image = decodeImageFile(path);
    if (!image.isValid()) {
        return {};
    }

    return createTextureFromRgba8(image.width, image.height, image.rgba8.data(), key);
}

TextureHandle RenderResourceManager::loadTexture(const std::string& path)
{
    if (path.empty()) {
        return {};
    }

    const std::string key = normalizeAssetPathKey(path);
    if (!key.empty()) {
        const auto cached = _textureCacheByKey.find(key);
        if (cached != _textureCacheByKey.end()) {
            TextureHandle out;
            out.id = cached->second;
            return out;
        }
    }

    ayt::resource::initializeLoaders();
    const std::shared_ptr<ayt::resource::IResource> resource =
        ayt::resource::ResourceRegistry::loadByPath(path);
    if (!resource) {
        return createTextureFromFile(path, key);
    }

    const auto* texture = dynamic_cast<const ayt::resource::ITexture*>(resource.get());
    if (texture == nullptr) {
        return {};
    }

    return uploadTextureFromResource(*this, *texture, key);
}

void RenderResourceManager::destroyTexture(TextureHandle& texture)
{
    if (!texture.isValid()) {
        texture = {};
        return;
    }

    removeTextureCacheEntry(texture.id);

    const auto it = _textures.find(texture.id);
    if (it != _textures.end()) {
        _adapter.destroy(it->second.handle);
        _textures.erase(it);
    }
    texture = {};
}

// Phase 1 SC-01: path-keyed lookup helpers. Use the same
// `normalizeAssetPathKey` helper as the internal loaders so
// `\` → `/` normalization matches what was cached.
MeshHandle RenderResourceManager::getMeshHandleByPath(const std::string& path) const
{
    if (path.empty()) return {};
    const auto it = _meshCacheByKey.find(normalizeAssetPathKey(path));
    if (it == _meshCacheByKey.end()) return {};
    return MeshHandle{ it->second };
}

MaterialHandle RenderResourceManager::getMaterialHandleByPath(const std::string& path) const
{
    if (path.empty()) return {};
    const auto it = _materialCacheByKey.find(normalizeAssetPathKey(path));
    if (it == _materialCacheByKey.end()) return {};
    return MaterialHandle{ it->second };
}

} // namespace ayt::render::detail
