#pragma once

#include "AYRenderTypes.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/GpuResources.h"

#include <string>
#include <unordered_map>

namespace ayt::render::detail
{

class RenderResourceManager {
public:
    RenderResourceManager(BGFXAdapter& adapter, shader::ShaderResourcePool& shaderPool);

    void shutdown();

    MeshHandle createMesh(const void* vertices,
                          uint32_t vertexCount,
                          const VertexLayoutDesc& layout,
                          const uint16_t* indices,
                          uint32_t indexCount);
    MeshHandle createMesh32(const void* vertices,
                            uint32_t vertexCount,
                            const VertexLayoutDesc& layout,
                            const uint32_t* indices,
                            uint32_t indexCount);
    MeshHandle createMeshFromResourceData(const void* vertices,
                                          uint32_t vertexCount,
                                          uint32_t vertexStride,
                                          const VertexLayoutDesc& layout,
                                          const uint32_t* indices,
                                          uint32_t indexCount);
    MeshHandle loadMesh(const std::string& path);
    MeshHandle createUnitCube();
    MeshHandle createTexturedUnitCube();
    void destroyMesh(MeshHandle& mesh);

    MaterialHandle createMaterialFromPhoskia(const std::string& source,
                                             const std::string& cacheKey = "");
    MaterialHandle createMaterialFromFile(const std::string& path);
    MaterialHandle loadMaterial(const std::string& path);
    void destroyMaterial(MaterialHandle& material);

    void setMaterialColor(MaterialHandle material, const char* propertyName,
                          float r, float g, float b, float a);
    void setMaterialFloat(MaterialHandle material, const char* uniformName, float value);
    void setMaterialVec2(MaterialHandle material, const char* uniformName,
                         float x, float y);
    void setMaterialVec3(MaterialHandle material, const char* uniformName,
                         float x, float y, float z);
    void setMaterialMatrix4(MaterialHandle material, const char* uniformName,
                            const ayt::math::Float4x4& matrix);
    void setMaterialTexture(MaterialHandle material, const char* textureBindingName,
                            TextureHandle texture);

    TextureHandle createTextureFromRgba8(uint32_t width, uint32_t height,
                                         const uint8_t* pixels,
                                         const std::string& cacheKey = "");
    TextureHandle createTextureFromData(uint32_t width, uint32_t height,
                                        uint32_t bgfxTextureFormat,
                                        const void* data, uint32_t size,
                                        const std::string& cacheKey = "");
    TextureHandle createTextureFromFile(const std::string& path,
                                        const std::string& cacheKey = "");
    TextureHandle loadTexture(const std::string& path);
    void destroyTexture(TextureHandle& texture);

    const std::unordered_map<uint64_t, GpuMesh>& meshes() const { return _meshes; }
    std::unordered_map<uint64_t, GpuMaterial>& materials() { return _materials; }
    const std::unordered_map<uint64_t, GpuMaterial>& materials() const { return _materials; }
    const std::unordered_map<uint64_t, GpuTexture>& textures() const { return _textures; }

private:
    BGFXAdapter&                 _adapter;
    shader::ShaderResourcePool&  _shaderPool;

    uint64_t _nextMeshId     = 1;
    uint64_t _nextMaterialId = 1;
    uint64_t _nextTextureId  = 1;

    std::unordered_map<uint64_t, GpuMesh>     _meshes;
    std::unordered_map<uint64_t, GpuMaterial> _materials;
    std::unordered_map<uint64_t, GpuTexture>  _textures;

    std::unordered_map<std::string, uint64_t> _materialCacheByKey;
    std::unordered_map<std::string, uint64_t> _textureCacheByKey;
    std::unordered_map<std::string, uint64_t> _meshCacheByKey;

    void releaseAllMaterials();
    void destroyAllMeshes();
    void destroyAllTextures();
    void removeMaterialCacheEntry(uint64_t id);
    void removeTextureCacheEntry(uint64_t id);
    void removeMeshCacheEntry(uint64_t id);

    MeshHandle uploadMeshInternal(const void* vertices,
                                  uint32_t vertexCount,
                                  uint32_t vertexStride,
                                  const VertexLayoutDesc& layout,
                                  const void* indices,
                                  uint32_t indexCount,
                                  bool use32BitIndices);
};

} // namespace ayt::render::detail
