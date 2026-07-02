#include "detail/RenderAssetBridge.h"

#include "detail/RenderResourceManager.h"

#include "AYAssetPath.h"
#include "assetsImpl/AYMaterial.h"

#include <bgfx/bgfx.h>

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
    case MaterialParamType::Float: {
        mgr.setMaterialFloat(handle, name, material.getFloat(name));
        break;
    }
    case MaterialParamType::Float2: {
        float values[2];
        material.getFloat2(name, values);
        mgr.setMaterialVec2(handle, name, values[0], values[1]);
        break;
    }
    case MaterialParamType::Float3: {
        const ayt::math::FVector3 value = material.getVector3(name);
        mgr.setMaterialVec3(handle, name, value.x, value.y, value.z);
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
    if (!addMeshFloatAttribute(out, mesh, MeshAttribute::Tangent, VertexAttribute::Tangent, 4)) {
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

namespace
{

bool mapAyTextureFormat(TextureFormat format, bgfx::TextureFormat::Enum& out)
{
    switch (format) {
    case TextureFormat::RGBA8: out = bgfx::TextureFormat::RGBA8; return true;
    case TextureFormat::RGB8:  out = bgfx::TextureFormat::RGB8;  return true;
    case TextureFormat::BC1:   out = bgfx::TextureFormat::BC1;   return true;
    case TextureFormat::BC3:   out = bgfx::TextureFormat::BC3;   return true;
    case TextureFormat::BC4:   out = bgfx::TextureFormat::BC4;   return true;
    case TextureFormat::BC5:   out = bgfx::TextureFormat::BC5;   return true;
    case TextureFormat::BC7:   out = bgfx::TextureFormat::BC7;   return true;
    default: return false;
    }
}

} // namespace

TextureHandle uploadTextureFromResource(RenderResourceManager& mgr,
                                          const ayt::resource::ITexture& texture,
                                          const std::string& cacheKey)
{
    if (texture.getMipmapCount() == 0) {
        return {};
    }

    const uint8_t* pixels = texture.getMipmapData(0);
    if (pixels == nullptr) {
        return {};
    }

    bgfx::TextureFormat::Enum bgfxFormat = bgfx::TextureFormat::RGBA8;
    if (!mapAyTextureFormat(texture.getFormat(), bgfxFormat)) {
        return {};
    }

    return mgr.createTextureFromData(texture.getWidth(),
                                     texture.getHeight(),
                                     static_cast<uint32_t>(bgfxFormat),
                                     pixels,
                                     texture.getMipmapSize(0),
                                     cacheKey);
}

} // namespace ayt::render::detail
