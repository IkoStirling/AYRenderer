#include "detail/RenderResourceManager.h"

#include "detail/RenderAssetBridge.h"
#include "detail/TextureImageLoader.h"
#include "detail/VertexLayoutBridge.h"
#include "detail/WorldLit2DMaterialSource.h"

#include "AYResource/AssetPath.h"
#include "AYResource/MeshMorphContract.h"
#include "AYResource/ResourceManager.h"
#include "AYResource/assetsDefs/IMaterial.h"
#include "AYResource/assetsDefs/IMesh.h"
#include "AYResource/assetsDefs/ITexture.h"

#include <AYIO/File.h>

#include <bgfx/bgfx.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace ayt::render::detail
{

namespace {

std::string textureSamplingCacheKey(const std::string& path, bool srgb)
{
    std::string key = normalizeAssetPathKey(path);
    if (!key.empty()) {
        key += srgb ? "#srgb" : "#linear";
    }
    return key;
}

// §P1 M1 (2026-08-24) — return bool + log on drop. Previously silent:
// a 128B array or null binding would silently not register, leaving
// the shader uniform un-set with no signal to the host. Now callers
// can react and the diag log tells the developer exactly which
// uniform was dropped and why (size / binding / null).
bool storeUniformSlot(GpuMaterial& material, const char* name,
                      shader::BindingId binding,
                      const void* data, size_t size)
{
    if (name == nullptr || binding == shader::InvalidBinding || data == nullptr
        || size == 0 || size > 64) {
        std::fprintf(stderr,
                     "[RenderResourceManager] storeUniformSlot dropped name=%s "
                     "binding=%u size=%zu (cause: %s)\n",
                     name ? name : "(null)",
                     static_cast<unsigned>(binding), size,
                     (name == nullptr) ? "null name"
                     : (binding == shader::InvalidBinding) ? "invalid binding"
                     : (data == nullptr) ? "null data"
                     : (size == 0) ? "zero size"
                     : "size > 64");
        return false;
    }

    for (GpuMaterial::UniformSlot& slot : material.uniformSlots) {
        if (slot.name == name) {
            slot.binding = binding;
            std::memcpy(slot.data, data, size);
            slot.size = static_cast<uint16_t>(size);
            return true;
        }
    }

    GpuMaterial::UniformSlot slot;
    slot.name    = name;
    slot.binding = binding;
    std::memcpy(slot.data, data, size);
    slot.size = static_cast<uint16_t>(size);
    material.uniformSlots.push_back(slot);
    return true;
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
    // Canonical CW front faces (geometric normals point outwards).
    4, 5, 6,  4, 6, 7,
    0, 2, 1,  0, 3, 2,
    1, 2, 6,  1, 6, 5,
    0, 4, 7,  0, 7, 3,
    3, 6, 2,  3, 7, 6,
    0, 1, 5,  0, 5, 4,
};

// CM-1 (2026-08-11) — unit quad in the XY plane. The 2D lane builds
// per-tile / per-sprite world matrices on top of this unit square
// (uv = tile source rect in the fragment shader via srcRect uniform).
const PosUvVertex kUnitQuadVertices[] = {
    {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f},
    { 0.5f, -0.5f, 0.0f, 1.0f, 0.0f},
    { 0.5f,  0.5f, 0.0f, 1.0f, 1.0f},
    {-0.5f,  0.5f, 0.0f, 0.0f, 1.0f},
};

const uint16_t kUnitQuadIndices[] = {
    0, 1, 2,  0, 2, 3,
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
    // Canonical CW front faces (geometric normals point outwards).
    0, 2, 1,  0, 3, 2,
    4, 5, 6,  4, 6, 7,
    8, 10, 9,  8, 11, 10,
    12, 14, 13,  12, 15, 14,
    16, 18, 17,  16, 19, 18,
    20, 22, 21,  20, 23, 22,
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
    for (auto it = _materials.begin(); it != _materials.end(); ++it) {
        if (it->second.shader.isValid()) {
            _shaderPool.release(it->second.shader);
        }
        it->second = GpuMaterial{};
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

void RenderResourceManager::destroyMeshGpuOnly(uint64_t id)
{
    const auto it = _meshes.find(id);
    if (it == _meshes.end()) {
        return;
    }
    _adapter.destroy(it->second.vertexBuffer);
    _adapter.destroy(it->second.indexBuffer);
    _meshes.erase(it);
}

void RenderResourceManager::destroyTextureGpuOnly(uint64_t id)
{
    const auto it = _textures.find(id);
    if (it == _textures.end()) {
        return;
    }
    _adapter.destroy(it->second.handle);
    _textures.erase(it);
}

void RenderResourceManager::destroyMaterialGpuOnly(uint64_t id)
{
    const auto it = _materials.find(id);
    if (it == _materials.end()) {
        return;
    }
    // §P1 M2 (2026-08-24) — release the ShaderResource too. Without
    // this, hot-reload paths that swap GPU-only (keep handle id) but
    // never go through destroyMaterial() leak shader compile slots in
    // the pool until shutdown drains. Also keeps the "GpuMaterial
    // owns a shader ref" invariant consistent with destroyMaterial().
    if (it->second.shader.isValid()) {
        _shaderPool.release(it->second.shader);
    }
    _materials.erase(it);
}

MeshHandle RenderResourceManager::uploadMeshInternal(const void* vertices,
                                                     uint32_t vertexCount,
                                                     uint32_t vertexStride,
                                                     const VertexLayoutDesc& layout,
                                                     const void* indices,
                                                     uint32_t indexCount,
                                                     bool use32BitIndices,
                                                     bool hasSkinWeights,
                                                     const MeshMorphContractDesc& morph)
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
    mesh.hasMorphTargets = morph.hasMorphTargets;
    mesh.morphTargetCount = morph.targetCount;
    mesh.morphDeltaCount = morph.deltaCount;
    mesh.morphPayloadChannels = morph.payloadChannels;
    mesh.morphContract = morph.contract;
    if (mesh.morphContract != nullptr) {
        mesh.morphWeights.reserve(mesh.morphContract->targets.size());
        for (const ayt::resource::MeshMorphTarget& target : mesh.morphContract->targets) {
            mesh.morphWeights.push_back(target.defaultWeight);
        }
    }
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
                                                             bool hasSkinWeights,
                                                             const MeshMorphContractDesc& morph)
{
    if (indices == nullptr || indexCount == 0) {
        return {};
    }

    // §P1 L1 (2026-08-24) — short-circuit the O(N) index scan when
    // the mesh has fewer than 65536 vertices. maxIndex cannot exceed
    // vertexCount-1 for any well-formed mesh, so the scan is
    // guaranteed-safe to skip. Hot-reload paths re-call this function
    // every frame for the same mesh, so the elimination matters more
    // than the bound itself.
    if (vertexCount >= 65536u) {
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
                                      indexCount, false, hasSkinWeights, morph);
        }
        return uploadMeshInternal(vertices, vertexCount, vertexStride, layout, indices,
                                  indexCount, true, hasSkinWeights, morph);
    }
    // vertexCount < 65536 ⇒ every index fits in uint16. Narrow
    // without scanning.
    std::vector<uint16_t> narrowed(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i) {
        narrowed[i] = static_cast<uint16_t>(indices[i]);
    }
    return uploadMeshInternal(vertices, vertexCount, vertexStride, layout, narrowed.data(),
                              indexCount, false, hasSkinWeights, morph);
}

MeshHandle RenderResourceManager::loadMesh(const std::string& assetReference)
{
    if (assetReference.empty()) {
        return {};
    }

    // Scene components store portable references. ResourceManager's loose
    // loader expects a filesystem path; resolve before loading and caching.
    const std::string path = ayt::resource::resolveAssetPath({}, assetReference);

    const std::string key = normalizeAssetPathKey(path);
    if (!key.empty()) {
        const auto cached = _meshCacheByKey.find(key);
        if (cached != _meshCacheByKey.end()) {
            MeshHandle out;
            out.id = cached->second;
            return out;
        }
    }

    // P0: L2 load must go through ResourceManager (cache/deps/pak/hot-reload).
    // Do not call ResourceRegistry::loadByPath from the renderer path.
    const std::shared_ptr<ayt::resource::IMesh> mesh =
        ayt::resource::ResourceManager::instance().load<ayt::resource::IMesh>(path);
    if (!mesh) {
        std::fprintf(stderr, "[RenderResourceManager] loadMesh failed: ResourceManager '%s'\n",
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

MeshHandle RenderResourceManager::createUnitQuad()
{
    if (!_adapter.isInitialized()) {
        return {};
    }
    return createMesh(kUnitQuadVertices,
                      static_cast<uint32_t>(sizeof(kUnitQuadVertices) / sizeof(PosUvVertex)),
                      VertexLayoutDesc::position3TexCoord2(),
                      kUnitQuadIndices,
                      static_cast<uint32_t>(sizeof(kUnitQuadIndices) / sizeof(uint16_t)));
}

void RenderResourceManager::destroyMesh(MeshHandle& mesh)
{
    if (!mesh.isValid()) {
        mesh = {};
        return;
    }

    removeMeshCacheEntry(mesh.id);
    // §P1 M7 (2026-08-24) — only the entry for this mesh was stale;
    // clearing the entire cache forced every other mesh to re-upload on
    // its next loadMesh(samePath). The path-keyed cache is dedup-safe
    // (one key = one mesh), so removing the dead entry is sufficient.
    // Full LRU eviction remains future work; until then any cross-key
    // stale lookup is no worse than before and we avoid the O(n) clear.

    const auto it = _meshes.find(mesh.id);
    if (it != _meshes.end()) {
        _adapter.destroy(it->second.vertexBuffer);
        _adapter.destroy(it->second.indexBuffer);
        _meshes.erase(it);
    }
    mesh = {};
}

bool RenderResourceManager::hasMorphTargets(MeshHandle mesh) const noexcept
{
    const auto it = _meshes.find(mesh.id);
    return it != _meshes.end() && it->second.hasMorphTargets
        && it->second.morphContract != nullptr;
}

uint32_t RenderResourceManager::morphTargetCount(MeshHandle mesh) const noexcept
{
    const auto it = _meshes.find(mesh.id);
    return it != _meshes.end() ? it->second.morphTargetCount : 0u;
}

std::string RenderResourceManager::morphTargetName(MeshHandle mesh, uint32_t targetIndex) const
{
    const auto it = _meshes.find(mesh.id);
    if (it == _meshes.end() || it->second.morphContract == nullptr
        || targetIndex >= it->second.morphContract->targets.size()) {
        return {};
    }
    return it->second.morphContract->targets[targetIndex].name;
}

bool RenderResourceManager::setMorphWeight(MeshHandle mesh,
                                           uint32_t targetIndex,
                                           float weight)
{
    const auto it = _meshes.find(mesh.id);
    if (it == _meshes.end() || !std::isfinite(weight)
        || targetIndex >= it->second.morphWeights.size()) {
        return false;
    }
    it->second.morphWeights[targetIndex] = weight;
    return true;
}

bool RenderResourceManager::setMorphWeight(MeshHandle mesh,
                                           const std::string& targetName,
                                           float weight)
{
    const auto it = _meshes.find(mesh.id);
    if (it == _meshes.end() || it->second.morphContract == nullptr
        || !std::isfinite(weight)) {
        return false;
    }
    const auto& targets = it->second.morphContract->targets;
    const auto targetIt = std::find_if(targets.begin(), targets.end(),
        [&](const ayt::resource::MeshMorphTarget& target) {
            return target.name == targetName;
        });
    if (targetIt == targets.end()) return false;
    return setMorphWeight(mesh, static_cast<uint32_t>(targetIt - targets.begin()), weight);
}

float RenderResourceManager::morphWeight(MeshHandle mesh, uint32_t targetIndex) const noexcept
{
    const auto it = _meshes.find(mesh.id);
    if (it == _meshes.end() || targetIndex >= it->second.morphWeights.size()) {
        return 0.0f;
    }
    return it->second.morphWeights[targetIndex];
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

MaterialHandle RenderResourceManager::createMaterialFromBgfxSc(const std::string& vertexSc,
                                                               const std::string& fragmentSc,
                                                               const std::string& varyingDefSc,
                                                               const std::string& cacheKey)
{
    MaterialHandle out;
    if (vertexSc.empty() || fragmentSc.empty() || varyingDefSc.empty()) {
        return out;
    }

    // Always allocate a NEW GpuMaterial instance. The shader pool still
    // dedupes compile artifacts via cacheKey+source; material instances
    // must stay unique so cube/ground (same .sc program, different
    // baseColor/albedo) cannot overwrite each other.
    std::fprintf(stderr, "[RenderResourceManager] compiling bgfx .sc material '%s'\n",
                 cacheKey.empty() ? "(anonymous)" : cacheKey.c_str());
    std::fflush(stderr);

    shader::ShaderResource shader =
        _shaderPool.acquireFromBgfxSc(vertexSc, fragmentSc, varyingDefSc, cacheKey);
    if (!shader.isValid()) {
        std::fprintf(stderr,
                     "[RenderResourceManager] createMaterialFromBgfxSc failed '%s'\n",
                     cacheKey.c_str());
        for (const std::string& err : _shaderPool.lastCompileErrors()) {
            std::fprintf(stderr, "  shader: %s\n", err.c_str());
        }
        std::fflush(stderr);
        return out;
    }

    const uint64_t id = _nextMaterialId++;
    GpuMaterial& mat = _materials.emplace(id, GpuMaterial{}).first->second;
    mat.shader = shader;
    out.id = id;
    std::fprintf(stderr,
                 "[RenderResourceManager] bgfx .sc material ready '%s' id=%llu\n",
                 cacheKey.c_str(),
                 static_cast<unsigned long long>(id));
    std::fflush(stderr);
    return out;
}

MaterialHandle RenderResourceManager::createMaterial2D(
    const Material2DDesc& desc, const std::string& cacheKey)
{
    if (!desc.albedo.isValid()
        || _textures.find(desc.albedo.id) == _textures.end()) {
        return {};
    }
    const auto isKnownOptionalTexture = [this](TextureHandle texture) {
        return !texture.isValid()
            || _textures.find(texture.id) != _textures.end();
    };
    if (!isKnownOptionalTexture(desc.normal)
        || !isKnownOptionalTexture(desc.roughnessMap)
        || !isKnownOptionalTexture(desc.emissiveMap)) {
        return {};
    }
    if (desc.alphaMode >= Material2DAlphaMode::Count) {
        return {};
    }

    MaterialHandle material = createMaterialFromPhoskia(
        kWorldLit2DMaterialPhoskiaSource, cacheKey);
    if (!material.isValid()) {
        return {};
    }

    setMaterialTexture(material, "albedoMap", desc.albedo);
    if (desc.normal.isValid()) {
        setMaterialTexture(material, "normalMap", desc.normal);
    }
    if (desc.roughnessMap.isValid()) {
        setMaterialTexture(material, "roughnessMap", desc.roughnessMap);
    }
    if (desc.emissiveMap.isValid()) {
        setMaterialTexture(material, "emissiveMap", desc.emissiveMap);
    }

    const auto finiteClamped = [](float value, float fallback,
                                  float minimum, float maximum) noexcept {
        return std::clamp(std::isfinite(value) ? value : fallback,
                          minimum, maximum);
    };
    const float metallic = finiteClamped(desc.metallic, 0.0f, 0.0f, 1.0f);
    const float roughness = finiteClamped(desc.roughness, 0.75f, 0.045f, 1.0f);
    const float ao = finiteClamped(desc.ambientOcclusion, 1.0f, 0.0f, 1.0f);
    const float emissive = finiteClamped(
        desc.emissiveStrength, 0.0f, 0.0f, 64.0f);
    const float alphaCutoff = finiteClamped(
        desc.alphaCutoff, 0.5f, 0.0f, 1.0f);

    setMaterialFloat(material, "metallic", metallic);
    setMaterialFloat(material, "roughness", roughness);
    setMaterialFloat(material, "ao", ao);
    setMaterialFloat(material, "normalYSign", desc.invertNormalY ? -1.0f : 1.0f);
    setMaterialVec3(material, "emissive", emissive, emissive, emissive);
    setMaterialModel(material, MaterialModel::StandardLit);
    setMaterialSurfaceProperties(
        material, static_cast<int>(desc.alphaMode), alphaCutoff,
        desc.doubleSided);
    return material;
}

void RenderResourceManager::resetMaterialBindingCache(GpuMaterial& material)
{
    material.colorBinding = shader::InvalidBinding;
    material.mat4Binding  = shader::InvalidBinding;
    material.boneBlockBinding = shader::InvalidBinding;
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

    // Always create a NEW GpuMaterial instance. Many .aymat files share one
    // .phoskia; caching materials by shader path made the second aymat
    // overwrite the first (cube + ground both became ground's gray-green,
    // same tex.idx, no distinct colors / broken shadow readback).
    // The ShaderResourcePool still shares the compiled GPU program.
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

uint32_t RenderResourceManager::reloadMaterialsForShaderFile(const std::string& shaderPath)
{
    if (shaderPath.empty()) {
        return 0;
    }

    const std::string key = normalizeAssetPathKey(shaderPath);
    uint32_t updated = 0;
    for (auto& [materialId, material] : _materials) {
        (void)materialId;
        if (material.shaderSourcePath.empty()) {
            continue;
        }
        if (normalizeAssetPathKey(material.shaderSourcePath) != key) {
            continue;
        }

        std::fprintf(stderr,
                     "[RenderResourceManager] reload shader '%s' for material id=%llu\n",
                     shaderPath.c_str(),
                     static_cast<unsigned long long>(materialId));

        shader::ShaderResource fresh = _shaderPool.compileFromFile(shaderPath);
        if (!fresh.isValid()) {
            for (const std::string& err : _shaderPool.lastCompileErrors()) {
                std::fprintf(stderr, "  shader reload: %s\n", err.c_str());
            }
            continue;
        }

        material.shader = fresh;
        rebindMaterialAfterShaderSwap(material);
        ++updated;
    }
    return updated;
}

bool RenderResourceManager::reloadMeshFromPath(const std::string& path)
{
    const std::string key = normalizeAssetPathKey(path);
    if (key.empty()) {
        return false;
    }
    const auto cached = _meshCacheByKey.find(key);
    if (cached == _meshCacheByKey.end()) {
        return false;
    }
    const uint64_t id = cached->second;

    const auto mesh = ayt::resource::ResourceManager::instance().load<ayt::resource::IMesh>(path);
    if (!mesh) {
        std::fprintf(stderr, "[RenderResourceManager] reloadMesh L2 miss '%s'\n", path.c_str());
        return false;
    }

    destroyMeshGpuOnly(id);
    const MeshHandle fresh = uploadMeshFromResource(*this, *mesh);
    if (!fresh.isValid()) {
        std::fprintf(stderr, "[RenderResourceManager] reloadMesh upload failed '%s'\n",
                     path.c_str());
        return false;
    }

    if (fresh.id != id) {
        _meshes[id] = std::move(_meshes.at(fresh.id));
        _meshes.erase(fresh.id);
    }
    _meshCacheByKey[key] = id;
    std::fprintf(stderr, "[RenderResourceManager] reloadMesh ok '%s' id=%llu\n",
                 path.c_str(), static_cast<unsigned long long>(id));
    return true;
}

bool RenderResourceManager::reloadMaterialFromPath(const std::string& path)
{
    const std::string key = normalizeAssetPathKey(path);
    if (key.empty()) {
        return false;
    }
    const auto cached = _materialCacheByKey.find(key);
    if (cached == _materialCacheByKey.end()) {
        return false;
    }
    const uint64_t id = cached->second;

    const auto material =
        ayt::resource::ResourceManager::instance().load<ayt::resource::IMaterial>(path);
    if (!material) {
        std::fprintf(stderr, "[RenderResourceManager] reloadMaterial L2 miss '%s'\n",
                     path.c_str());
        return false;
    }

    // Drop path cache so bindMaterialFromResource can create a fresh GPU mat,
    // then remap onto the stable handle id so scene refs stay valid.
    _materialCacheByKey.erase(cached);
    destroyMaterialGpuOnly(id);

    MaterialHandle fresh = bindMaterialFromResource(*this, *material, path);
    if (!fresh.isValid()) {
        std::fprintf(stderr, "[RenderResourceManager] reloadMaterial bind failed '%s'\n",
                     path.c_str());
        return false;
    }

    if (fresh.id != id) {
        _materials[id] = std::move(_materials.at(fresh.id));
        _materials.erase(fresh.id);
    }
    _materialCacheByKey[key] = id;
    std::fprintf(stderr, "[RenderResourceManager] reloadMaterial ok '%s' id=%llu\n",
                 path.c_str(), static_cast<unsigned long long>(id));
    return true;
}

bool RenderResourceManager::reloadTextureFromPath(const std::string& path)
{
    if (normalizeAssetPathKey(path).empty()) {
        return false;
    }

    const auto texture =
        ayt::resource::ResourceManager::instance().load<ayt::resource::ITexture>(path);
    bool refreshed = false;
    for (const bool srgb : {false, true}) {
        const std::string key = textureSamplingCacheKey(path, srgb);
        const auto cached = _textureCacheByKey.find(key);
        if (cached == _textureCacheByKey.end()) {
            continue;
        }
        const uint64_t id = cached->second;
        destroyTextureGpuOnly(id);
        _textureCacheByKey.erase(cached);

        const TextureHandle fresh = texture
            ? uploadTextureFromResource(*this, *texture, key, srgb)
            : createTextureFromFile(path, key, srgb);
        if (!fresh.isValid()) {
            std::fprintf(stderr,
                         "[RenderResourceManager] reloadTexture upload failed '%s' (%s)\n",
                         path.c_str(), srgb ? "sRGB" : "linear");
            continue;
        }
        if (fresh.id != id) {
            _textures[id] = std::move(_textures.at(fresh.id));
            _textures.erase(fresh.id);
            _textureCacheByKey[key] = id;
        }
        std::fprintf(stderr,
                     "[RenderResourceManager] reloadTexture ok '%s' id=%llu (%s)\n",
                     path.c_str(), static_cast<unsigned long long>(id),
                     srgb ? "sRGB" : "linear");
        refreshed = true;
    }
    return refreshed;
}

bool RenderResourceManager::onResourceFileChanged(const std::string& path)
{
    if (path.empty()) {
        return false;
    }

    const std::string key = normalizeAssetPathKey(path);
    const auto dot = key.find_last_of('.');
    const std::string ext = (dot == std::string::npos) ? std::string{} : key.substr(dot);

    if (ext == ".aymesh") {
        return reloadMeshFromPath(path);
    }
    if (ext == ".aymat") {
        return reloadMaterialFromPath(path);
    }
    if (ext == ".aytex" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga"
        || ext == ".bmp" || ext == ".hdr") {
        return reloadTextureFromPath(path);
    }

    // Unknown extension: try whichever path cache has it.
    if (_meshCacheByKey.count(key) != 0 && reloadMeshFromPath(path)) {
        return true;
    }
    if (_materialCacheByKey.count(key) != 0 && reloadMaterialFromPath(path)) {
        return true;
    }
    if (_textureCacheByKey.count(key) != 0 && reloadTextureFromPath(path)) {
        return true;
    }
    return false;
}

MaterialHandle RenderResourceManager::loadMaterial(const std::string& assetReference)
{
    if (assetReference.empty()) {
        return {};
    }

    const std::string path = ayt::resource::resolveAssetPath({}, assetReference);

    const std::string key = normalizeAssetPathKey(path);
    if (!key.empty()) {
        const auto cached = _materialCacheByKey.find(key);
        if (cached != _materialCacheByKey.end()) {
            MaterialHandle out;
            out.id = cached->second;
            return out;
        }
    }

    // P0: L2 load must go through ResourceManager (cache/deps/pak/hot-reload).
    const std::shared_ptr<ayt::resource::IMaterial> material =
        ayt::resource::ResourceManager::instance().load<ayt::resource::IMaterial>(path);
    if (!material) {
        std::fprintf(stderr, "[RenderResourceManager] loadMaterial failed: ResourceManager '%s'\n",
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
    const shader::BindingId binding = mat.shader.getUniformBinding(propertyName);
    const float value[4] = {r, g, b, a};

    // colorOverride is the canonical BaseColor fallback consumed by GBuffer
    // and the alpha-mask shadow caster. Other Float4 properties (emissive,
    // opacity, etc.) must retain independent slots; treating the last Float4
    // as BaseColor made parameter iteration order change the rendered model.
    if (std::strcmp(propertyName, "baseColor") == 0
        || std::strcmp(propertyName, "color") == 0) {
        mat.colorOverride = ayt::math::FVector4(r, g, b, a);
        mat.hasColorOverride = true;
        mat.colorBinding = binding != shader::InvalidBinding
            ? binding : mat.shader.getUniformBinding("baseColor");
        return;
    }

    storeUniformSlot(mat, propertyName, binding, value, sizeof(value));
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
    // bgfx represents float/vec2/vec3 uniforms as one 16-byte Vec4 slot.
    // Passing only four bytes makes bgfx read beyond the stored payload and
    // turns imported metallic/roughness values into nondeterministic data.
    const float padded[4] = {value, 0.0f, 0.0f, 0.0f};
    storeUniformSlot(mat, uniformName, binding, padded, sizeof(padded));
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

    const float values[4] = {x, y, 0.0f, 0.0f};
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

void RenderResourceManager::setMaterialSurfaceProperties(MaterialHandle material,
                                                         int alphaMode,
                                                         float alphaCutoff,
                                                         bool doubleSided)
{
    if (!material.isValid()) {
        return;
    }

    const auto it = _materials.find(material.id);
    if (it == _materials.end()) {
        return;
    }

    GpuMaterial& mat = it->second;
    mat.alphaCutout = alphaMode == 1;
    mat.blendMode = alphaMode == 2 ? BlendMode::Alpha : BlendMode::Opaque;
    mat.alphaCutoff = std::clamp(alphaCutoff, 0.0f, 1.0f);
    mat.doubleSided = doubleSided;

    // PBR/forward shaders use this value to orient thin-sheet normals toward
    // the viewer when culling is disabled. Store it as a regular per-draw
    // vec4 upload so every material program sees the same surface contract.
    GpuMaterial::UniformSlot* surfaceSlot = nullptr;
    for (GpuMaterial::UniformSlot& slot : mat.uniformSlots) {
        if (slot.name == "doubleSided") {
            surfaceSlot = &slot;
            break;
        }
    }
    if (surfaceSlot == nullptr) {
        mat.uniformSlots.push_back({});
        surfaceSlot = &mat.uniformSlots.back();
        surfaceSlot->name = "doubleSided";
    }
    const float value[4] = {doubleSided ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    std::memcpy(surfaceSlot->data, value, sizeof(value));
    surfaceSlot->size = static_cast<uint16_t>(sizeof(value));
}

void RenderResourceManager::setMaterialPremultipliedAlpha(MaterialHandle material,
                                                          bool premultiplied)
{
    if (!material.isValid()) {
        return;
    }
    const auto it = _materials.find(material.id);
    if (it != _materials.end()) {
        it->second.premultipliedAlpha = premultiplied;
    }
}

bool RenderResourceManager::setMaterialBlendMode(MaterialHandle material,
                                                 BlendMode blendMode)
{
    if (!material.isValid()) {
        return false;
    }
    const auto it = _materials.find(material.id);
    if (it == _materials.end()) {
        return false;
    }
    // §P1 H2 (2026-08-24) — blend mode drives forward/transparent pass
    // routing at submit time. Callers that flip Opaque↔AlphaBlend must
    // re-add the DrawItem to RenderScene so the next frame's pass
    // selection sees the new value (no global re-routing pass exists).
    it->second.blendMode = blendMode;
    return true;
}

bool RenderResourceManager::setMaterialModel(MaterialHandle material,
                                             MaterialModel model)
{
    if (!material.isValid()
        || static_cast<uint8_t>(model)
            >= static_cast<uint8_t>(MaterialModel::Count)) {
        return false;
    }
    const auto it = _materials.find(material.id);
    if (it == _materials.end()) {
        return false;
    }
    it->second.materialModel = model;
    return true;
}

TextureHandle RenderResourceManager::createTextureFromRgba8(uint32_t width, uint32_t height,
                                                            const uint8_t* pixels,
                                                            const std::string& cacheKey)
{
    return createTextureFromRgba8(width, height, pixels, cacheKey, false);
}

TextureHandle RenderResourceManager::createTextureFromRgba8(uint32_t width, uint32_t height,
                                                            const uint8_t* pixels,
                                                            const std::string& cacheKey,
                                                            bool srgb)
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
    gpuTex.srgb = srgb;
    const uint64_t flags = srgb ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE;
    gpuTex.handle = _adapter.createTexture2D(gpuTex.width, gpuTex.height, pixels, flags);
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

TextureHandle RenderResourceManager::createDynamicTextureRgba8(uint32_t width,
                                                               uint32_t height)
{
    TextureHandle out;
    if (!_adapter.isInitialized() || width == 0 || height == 0) {
        return out;
    }

    GpuTexture gpuTex;
    gpuTex.width  = static_cast<uint16_t>(width);
    gpuTex.height = static_cast<uint16_t>(height);
    gpuTex.dynamic = true;
    gpuTex.handle = _adapter.createDynamicTexture2D(gpuTex.width, gpuTex.height);
    if (!bgfx::isValid(gpuTex.handle)) {
        return out;
    }

    const uint64_t id = _nextTextureId++;
    _textures.emplace(id, gpuTex);
    out.id = id;
    return out;
}

bool RenderResourceManager::updateTextureFromRgba8(TextureHandle texture,
                                                   const uint8_t* pixels)
{
    if (!_adapter.isInitialized() || texture.id == 0 || pixels == nullptr) {
        return false;
    }
    const auto it = _textures.find(texture.id);
    if (it == _textures.end()) {
        return false;
    }
    GpuTexture& gpuTex = it->second;
    if (!gpuTex.dynamic || !bgfx::isValid(gpuTex.handle)
        || gpuTex.width == 0 || gpuTex.height == 0) {
        return false;
    }
    _adapter.updateTexture2D(gpuTex.handle, gpuTex.width, gpuTex.height, pixels);
    return true;
}

TextureHandle RenderResourceManager::createCubeTextureFromRgba8(uint32_t size,
                                                                const uint8_t* rgba8Faces,
                                                                const std::string& cacheKey)
{
    TextureHandle out;
    if (!_adapter.isInitialized() || size == 0 || rgba8Faces == nullptr) {
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
    gpuTex.width  = static_cast<uint16_t>(size);
    gpuTex.height = static_cast<uint16_t>(size);
    gpuTex.handle = _adapter.createTextureCube(gpuTex.width, rgba8Faces);
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
    return createTextureFromData(width, height, bgfxTextureFormat, data, size,
                                 cacheKey, false);
}

TextureHandle RenderResourceManager::createTextureFromData(uint32_t width, uint32_t height,
                                                           uint32_t bgfxTextureFormat,
                                                           const void* data, uint32_t size,
                                                           const std::string& cacheKey,
                                                           bool srgb)
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
    gpuTex.srgb = srgb;
    const uint64_t flags = srgb ? BGFX_TEXTURE_SRGB : BGFX_TEXTURE_NONE;
    gpuTex.handle = _adapter.createTexture2DFromData(
        gpuTex.width, gpuTex.height,
        static_cast<bgfx::TextureFormat::Enum>(bgfxTextureFormat),
        data, size, flags);
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
    return createTextureFromFile(path, cacheKey, false);
}

TextureHandle RenderResourceManager::createTextureFromFile(const std::string& path,
                                                           const std::string& cacheKey,
                                                           bool srgb)
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

    return createTextureFromRgba8(image.width, image.height, image.rgba8.data(), key, srgb);
}

TextureHandle RenderResourceManager::loadTexture(const std::string& path)
{
    return loadTexture(path, false);
}

TextureHandle RenderResourceManager::loadTexture(const std::string& path, bool srgb)
{
    if (path.empty()) {
        return {};
    }

    const std::string key = textureSamplingCacheKey(path, srgb);
    if (!key.empty()) {
        const auto cached = _textureCacheByKey.find(key);
        if (cached != _textureCacheByKey.end()) {
            TextureHandle out;
            out.id = cached->second;
            return out;
        }
    }

    // P0: L2 load must go through ResourceManager (cache/deps/pak/hot-reload).
    // Fall back to raw image decode when the path is not a typed .ay* asset.
    const std::shared_ptr<ayt::resource::ITexture> texture =
        ayt::resource::ResourceManager::instance().load<ayt::resource::ITexture>(path);
    if (!texture) {
        return createTextureFromFile(path, key, srgb);
    }

    return uploadTextureFromResource(*this, *texture, key, srgb);
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
    const auto it = _meshCacheByKey.find(normalizeAssetPathKey(
        ayt::resource::resolveAssetPath({}, path)));
    if (it == _meshCacheByKey.end()) return {};
    return MeshHandle{ it->second };
}

MaterialHandle RenderResourceManager::getMaterialHandleByPath(const std::string& path) const
{
    if (path.empty()) return {};
    const auto it = _materialCacheByKey.find(normalizeAssetPathKey(
        ayt::resource::resolveAssetPath({}, path)));
    if (it == _materialCacheByKey.end()) return {};
    return MaterialHandle{ it->second };
}

} // namespace ayt::render::detail
