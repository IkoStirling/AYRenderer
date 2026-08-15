#pragma once

#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/GpuResources.h"

#include <string>
#include <unordered_map>

namespace ayt::render::detail
{

// L3 owner: GPU maps + opaque Mesh/Material/Texture handles.
// Upload copies L2 CPU data; RRM does not retain shared_ptr<IResource>.
// L2 unload/trim does not destroy GPU — only destroy* / shutdown / hot-reload
// path refresh (stable handle ids). See AYResource/docs/ownership-contracts.md.
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
                                          uint32_t indexCount,
                                          bool hasSkinWeights = false);
    MeshHandle loadMesh(const std::string& path);
    MeshHandle createUnitCube();
    MeshHandle createTexturedUnitCube();
    // CM-1 (2026-08-11) — unit quad (XY plane, z=0), UV (0,0)..(1,1).
    MeshHandle createUnitQuad();
    void destroyMesh(MeshHandle& mesh);

    MaterialHandle createMaterialFromPhoskia(const std::string& source,
                                             const std::string& cacheKey = "");
    MaterialHandle createMaterialFromBgfxSc(const std::string& vertexSc,
                                            const std::string& fragmentSc,
                                            const std::string& varyingDefSc,
                                            const std::string& cacheKey = "");
    MaterialHandle createMaterialFromFile(const std::string& path);
    MaterialHandle loadMaterial(const std::string& path);
    void destroyMaterial(MaterialHandle& material);

    // Re-compile materials whose ShaderResource was invalidated by pool hot-reload.
    void refreshMaterialsAfterHotReload();

    // Re-compile every loaded material that was built from `shaderPath`
    // (normalized path compare). Returns the number of materials updated.
    uint32_t reloadMaterialsForShaderFile(const std::string& shaderPath);

    // P2: L2 file changed → drop GPU for that path (keep handle id) and re-upload.
    // No-op when the path was never GPU-cached. Returns true if something refreshed.
    bool onResourceFileChanged(const std::string& path);
    bool reloadMeshFromPath(const std::string& path);
    bool reloadMaterialFromPath(const std::string& path);
    bool reloadTextureFromPath(const std::string& path);

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
    // Mutable RGBA8 texture for per-frame CPU uploads (AYVideo V3).
    // Never cached — each call allocates a fresh GPU texture.
    TextureHandle createDynamicTextureRgba8(uint32_t width, uint32_t height);
    // Full-rect rewrite. Returns false on invalid handle / size mismatch /
    // non-dynamic texture / null pixels.
    bool updateTextureFromRgba8(TextureHandle texture, const uint8_t* pixels);
    // §P5.5 D-upload — RGBA8 cubemap. `rgba8Faces` layout matches
    // bgfx::createTextureCube (6 faces × size² × 4).
    TextureHandle createCubeTextureFromRgba8(uint32_t size,
                                             const uint8_t* rgba8Faces,
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

    // Phase 1 SC-01: path-keyed lookup. Returns an invalid handle
    // when the path was never loaded. Use this from gameplay/ECS
    // code (e.g. SkinnedMeshRenderSystem) to reuse the existing
    // GPU mesh/material that loadMesh/loadMaterial already cached.
    // Both lookups are O(1) against `_meshCacheByKey` /
    // `_materialCacheByKey`. Normalizes `\` → `/` so the same
    // physical path resolves identically regardless of separator.
    MeshHandle    getMeshHandleByPath(const std::string& path) const;
    MaterialHandle getMaterialHandleByPath(const std::string& path) const;

    // RD-07: cache-size introspection for unit tests / diagnostics.
    // Lets RD-07 tests assert "N unique paths → N cache entries" and
    // "second loadMesh of same path doesn't grow the cache". These
    // are O(1) and safe to call from release builds.
    size_t meshCacheSize() const     { return _meshCacheByKey.size(); }
    size_t materialCacheSize() const { return _materialCacheByKey.size(); }

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
    void destroyMeshGpuOnly(uint64_t id);
    void destroyTextureGpuOnly(uint64_t id);
    void destroyMaterialGpuOnly(uint64_t id);

    void resetMaterialBindingCache(GpuMaterial& material);
    void rebindMaterialAfterShaderSwap(GpuMaterial& material);

    MeshHandle uploadMeshInternal(const void* vertices,
                                  uint32_t vertexCount,
                                  uint32_t vertexStride,
                                  const VertexLayoutDesc& layout,
                                  const void* indices,
                                  uint32_t indexCount,
                                  bool use32BitIndices,
                                  bool hasSkinWeights = false);
};

} // namespace ayt::render::detail
