#include "detail/RenderAssetBridge.h"

#include "detail/RenderResourceManager.h"

#include "AYAssetPath.h"
#include "assetsImpl/AYMaterial.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ayt::render::detail
{

namespace
{

using ayt::resource::MaterialParamType;
using ayt::resource::MeshAttribute;
using ayt::resource::TextureFormat;

bool addMeshFloatAttribute(VertexLayoutDesc& layout,
                           const ayt::resource::IMesh& mesh,
                           MeshAttribute meshAttr,
                           VertexAttribute renderAttr,
                           uint8_t componentCount)
{
    if (!mesh.hasAttribute(meshAttr)) {
        return true;
    }

    VertexElement element{};
    element.attribute       = renderAttr;
    element.componentCount  = componentCount;
    element.componentType   = VertexComponentType::Float;
    element.normalized      = false;
    return layout.add(element);
}

void applyMaterialParameter(RenderResourceManager& mgr,
                            const ayt::resource::IMaterial& material,
                            const std::string& materialPath,
                            MaterialHandle handle,
                            const char* name,
                            MaterialParamType type)
{
    switch (type) {
    case MaterialParamType::Float4: {
        const ayt::math::FVector4 value = material.getVector4(name);
        mgr.setMaterialColor(handle, name, value.x, value.y, value.z, value.w);
        break;
    }
    case MaterialParamType::Float3: {
        const ayt::math::FVector3 value = material.getVector3(name);
        mgr.setMaterialColor(handle, name, value.x, value.y, value.z, 1.0f);
        break;
    }
    case MaterialParamType::Float4x4: {
        const float* matrix = material.getMatrix(name);
        if (matrix != nullptr) {
            ayt::math::Float4x4 value;
            std::memcpy(value.ptr(), matrix, sizeof(float) * 16);
            mgr.setMaterialMatrix4(handle, name, value);
        }
        break;
    }
    case MaterialParamType::Texture2D:
    case MaterialParamType::Texture3D:
    case MaterialParamType::TextureCube: {
        const char* textureRef = material.getTexture(name);
        if (textureRef == nullptr || textureRef[0] == '\0') {
            break;
        }
        const std::string texturePath =
            ayt::resource::resolveAssetPath(materialPath, textureRef);
        const TextureHandle texture = mgr.loadTexture(texturePath);
        if (texture.isValid()) {
            mgr.setMaterialTexture(handle, name, texture);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

std::string normalizeAssetPathKey(const std::string& path)
{
    std::string key = path;
    for (char& ch : key) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    return key;
}

bool vertexLayoutFromMesh(const ayt::resource::IMesh& mesh, VertexLayoutDesc& out)
{
    const uint8_t mask = mesh.getAttributeMask();
    if ((mask & (1u << static_cast<uint8_t>(MeshAttribute::Tangent))) != 0) {
        return false;
    }

    out = VertexLayoutDesc{};

    if (!addMeshFloatAttribute(out, mesh, MeshAttribute::Position, VertexAttribute::Position, 3)) {
        return false;
    }
    if (!addMeshFloatAttribute(out, mesh, MeshAttribute::Normal, VertexAttribute::Normal, 3)) {
        return false;
    }
    if (!addMeshFloatAttribute(out, mesh, MeshAttribute::UV, VertexAttribute::TexCoord0, 2)) {
        return false;
    }
    if (!addMeshFloatAttribute(out, mesh, MeshAttribute::Color, VertexAttribute::Color0, 4)) {
        return false;
    }

    if (!out.isValid()) {
        return false;
    }
    return out.strideBytes() == mesh.getVertexStride();
}

MeshHandle uploadMeshFromResource(RenderResourceManager& mgr,
                                  const ayt::resource::IMesh& mesh)
{
    VertexLayoutDesc layout;
    if (!vertexLayoutFromMesh(mesh, layout)) {
        return {};
    }

    if (mesh.getVertexCount() == 0 || mesh.getIndexCount() == 0
        || mesh.getVertexData() == nullptr || mesh.getIndexData() == nullptr) {
        return {};
    }

    return mgr.createMeshFromResourceData(mesh.getVertexData(),
                                          mesh.getVertexCount(),
                                          layout,
                                          mesh.getIndexData(),
                                          mesh.getIndexCount());
}

MaterialHandle bindMaterialFromResource(RenderResourceManager& mgr,
                                        const ayt::resource::IMaterial& material,
                                        const std::string& materialPath)
{
    const char* shaderRef = material.getShader();
    if (shaderRef == nullptr || shaderRef[0] == '\0') {
        return {};
    }

    const std::string shaderPath =
        ayt::resource::resolveAssetPath(materialPath, shaderRef);
    MaterialHandle handle = mgr.createMaterialFromFile(shaderPath);
    if (!handle.isValid()) {
        return {};
    }

    if (const auto* concrete = dynamic_cast<const ayt::resource::Material*>(&material)) {
        concrete->forEachParameter([&](const char* name, MaterialParamType type) {
            applyMaterialParameter(mgr, material, materialPath, handle, name, type);
        });
    }

    return handle;
}

TextureHandle uploadTextureFromResource(RenderResourceManager& mgr,
                                          const ayt::resource::ITexture& texture,
                                          const std::string& cacheKey)
{
    if (texture.getFormat() != TextureFormat::RGBA8) {
        return {};
    }
    if (texture.getMipmapCount() == 0) {
        return {};
    }

    const uint8_t* pixels = texture.getMipmapData(0);
    if (pixels == nullptr) {
        return {};
    }

    return mgr.createTextureFromRgba8(texture.getWidth(),
                                      texture.getHeight(),
                                      pixels,
                                      cacheKey);
}

} // namespace ayt::render::detail
