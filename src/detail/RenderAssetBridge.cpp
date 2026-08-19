#include "detail/RenderAssetBridge.h"

#include "detail/RenderResourceManager.h"
#include "detail/VertexLayoutBridge.h"

#include "AYResource/AssetPath.h"
#include "AYRenderer/ShadowShaderSources.h"
#include "AYResource/assetsImpl/Material.h"
#include <AYIO/Env.h>
#include <AYIO/File.h>
#include <AYIO/Path.h>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        } else {
            std::fprintf(stderr,
                         "[RenderAssetBridge] loadTexture failed (param='%s' path='%s' mat='%s')\n",
                         name, texturePath.c_str(), materialPath.c_str());
        }
        break;
    }
    default:
        break;
    }
}

// Bare shader filenames resolve base-relative by convention, but the
// editor ships .phoskia sources under the asset root (not beside each
// .aymat in materials/). Try the base-relative hit first, then the
// root; returns the first path that exists, else the base-relative one
// (caller logs the miss).
std::string resolveShaderPath(const std::string& materialPath,
                              const std::string& shaderRef)
{
    const std::string baseHit =
        ayt::resource::resolveAssetPath(materialPath, shaderRef);
    if (ayt::io::File::exists(baseHit)) {
        return baseHit;
    }
    const std::string& root = ayt::resource::assetRoot();
    if (!root.empty() && !ayt::io::path::isAbsolute(shaderRef)) {
        const std::string rootHit = ayt::io::path::join(root, shaderRef);
        if (ayt::io::File::exists(rootHit)) {
            return rootHit;
        }
    }
    return baseHit;
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

bool buildBgfxVertexLayoutFromMesh(const ayt::resource::IMesh& mesh,
                                   VertexLayoutDesc& desc,
                                   bgfx::VertexLayout& bgfxLayout)
{
    if (!vertexLayoutFromMesh(mesh, desc)) {
        return false;
    }
    return buildBgfxVertexLayout(desc, bgfxLayout);
}

bool vertexLayoutFromMesh(const ayt::resource::IMesh& mesh, VertexLayoutDesc& out)
{
    out = VertexLayoutDesc{};

    if (!mesh.hasAttribute(MeshAttribute::Position)) {
        return false;
    }

    // Phase 0 fast-path: when SkinWeight is present we must NOT use the posNormalUv
    // preset (it's a 3-element layout with no room for skin channels). Fall through
    // to the per-attribute path which can append skin channels.
    const bool hasSkin = mesh.hasAttribute(MeshAttribute::SkinWeight);

    const bool isPosNormalUv = !hasSkin
        && mesh.hasAttribute(MeshAttribute::Normal)
        && mesh.hasAttribute(MeshAttribute::UV)
        && !mesh.hasAttribute(MeshAttribute::Tangent)
        && !mesh.hasAttribute(MeshAttribute::Color);
    if (isPosNormalUv) {
        out = VertexLayoutDesc::position3Normal3TexCoord2();
        return out.isValid();
    }

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

    // Phase 0 RD-02: append skin channels last (deterministic order).
    // The skinnedAddon() preset adds BoneIndices (4x u8 normalized) + BoneWeights (4x f32).
    if (hasSkin) {
        const VertexLayoutDesc addon = VertexLayoutDesc::skinnedAddon();
        if (!out.add(addon.elements[0]) || !out.add(addon.elements[1])) {
            return false;
        }
    }

    return out.isValid();
}

bool meshNeedsVertexRepack(const ayt::resource::IMesh& mesh,
                           const bgfx::VertexLayout& bgfxLayout)
{
    if (bgfxLayout.getStride() != mesh.getVertexStride()) {
        return true;
    }

    struct ChannelMap {
        MeshAttribute      meshAttr;
        bgfx::Attrib::Enum bgfxAttr;
    };

    static const ChannelMap kChannels[] = {
        {MeshAttribute::Position, bgfx::Attrib::Position},
        {MeshAttribute::Normal,   bgfx::Attrib::Normal},
        {MeshAttribute::UV,       bgfx::Attrib::TexCoord0},
        {MeshAttribute::Tangent,  bgfx::Attrib::Tangent},
        {MeshAttribute::Color,    bgfx::Attrib::Color0},
    };

    for (const ChannelMap& channel : kChannels) {
        if (!mesh.hasAttribute(channel.meshAttr)) {
            continue;
        }
        if (!bgfxLayout.has(channel.bgfxAttr)) {
            return true;
        }
        const uint16_t dstOffset = bgfxLayout.getOffset(channel.bgfxAttr);
        const uint8_t srcOffset  = mesh.getAttributeInfo(channel.meshAttr).offset;
        if (dstOffset != srcOffset) {
            return true;
        }
    }

    return false;
}

bool repackMeshVertices(const ayt::resource::IMesh& mesh,
                        const bgfx::VertexLayout& bgfxLayout,
                        std::vector<uint8_t>& out)
{
    const uint32_t srcStride    = mesh.getVertexStride();
    const uint32_t dstStride    = bgfxLayout.getStride();
    const uint32_t vertexCount  = mesh.getVertexCount();
    const uint8_t* srcBase      = mesh.getVertexData();

    if (srcStride == 0 || dstStride == 0 || vertexCount == 0 || srcBase == nullptr) {
        return false;
    }

    out.resize(static_cast<size_t>(dstStride) * vertexCount);
    std::memset(out.data(), 0, out.size());

    struct ChannelMap {
        MeshAttribute         meshAttr;
        bgfx::Attrib::Enum    bgfxAttr;
        uint8_t               floatCount;
    };

    // Phase 0 RD-02: the SkinWeight mesh attr maps to TWO bgfx channels
    // (Indices u8 normalized + Weight f32, in this bgfx fork). Listed here so
    // the loop handles both halves in deterministic order.
    static const ChannelMap kChannels[] = {
        {MeshAttribute::Position,   bgfx::Attrib::Position,   3},
        {MeshAttribute::Normal,     bgfx::Attrib::Normal,     3},
        {MeshAttribute::UV,         bgfx::Attrib::TexCoord0,  2},
        {MeshAttribute::Tangent,    bgfx::Attrib::Tangent,    4},
        {MeshAttribute::Color,      bgfx::Attrib::Color0,     4},
        {MeshAttribute::SkinWeight, bgfx::Attrib::Indices,    4},
        {MeshAttribute::SkinWeight, bgfx::Attrib::Weight,     4},
    };

    // Get the IMesh-side skin weight table once; null if mesh has no skin weights.
    const ayt::resource::VertexSkinWeight* skinWeights = mesh.getSkinWeights();

    for (uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
        const uint8_t* srcVertex = srcBase + static_cast<size_t>(vertexIndex) * srcStride;

        for (const ChannelMap& channel : kChannels) {
            if (!mesh.hasAttribute(channel.meshAttr) || !bgfxLayout.has(channel.bgfxAttr)) {
                continue;
            }

            // SkinWeight channel: read from getSkinWeights() (separate buffer) not
            // from the interleaved vertex stream.
            if (channel.meshAttr == MeshAttribute::SkinWeight) {
                if (skinWeights == nullptr) {
                    continue;
                }
                if (channel.bgfxAttr == bgfx::Attrib::Indices) {
                    // bgfx::vertexPack with normalized=true expects the value in
                    // [0,1] and re-encodes as uint8 = value * 255. IMesh stores
                    // raw uint8 boneIndex; divide by 255 here so pack produces
                    // the same byte.
                    const uint8_t* src = skinWeights[vertexIndex].boneIndex;
                    constexpr float kInv255 = 1.0f / 255.0f;
                    float values[4] = {
                        static_cast<float>(src[0]) * kInv255,
                        static_cast<float>(src[1]) * kInv255,
                        static_cast<float>(src[2]) * kInv255,
                        static_cast<float>(src[3]) * kInv255
                    };
                    bgfx::vertexPack(values, true, channel.bgfxAttr, bgfxLayout,
                                     out.data(), vertexIndex);
                } else {
                    // Weight (in this bgfx fork): 4 x f32 direct memcpy.
                    float values[4] = {
                        skinWeights[vertexIndex].boneWeight[0],
                        skinWeights[vertexIndex].boneWeight[1],
                        skinWeights[vertexIndex].boneWeight[2],
                        skinWeights[vertexIndex].boneWeight[3]
                    };
                    bgfx::vertexPack(values, false, channel.bgfxAttr, bgfxLayout,
                                     out.data(), vertexIndex);
                }
                continue;
            }

            const uint8_t srcOffset = mesh.getAttributeInfo(channel.meshAttr).offset;
            float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            std::memcpy(values,
                        srcVertex + srcOffset,
                        static_cast<size_t>(channel.floatCount) * sizeof(float));
            bgfx::vertexPack(values,
                             false,
                             channel.bgfxAttr,
                             bgfxLayout,
                             out.data(),
                             vertexIndex);
        }
    }

    return true;
}

MeshHandle uploadMeshFromResource(RenderResourceManager& mgr,
                                  const ayt::resource::IMesh& mesh)
{
    // RD diagnostic: opt-in per-call counter + wall-clock timing
    // when AY_RENDERER_UPLOAD_TIMING is set. Counts how many times
    // uploadMeshFromResource is called per editor frame; if it is
    // called >1x/frame on a stable scene, something is forcing the
    // scene-bridge to re-decode or re-upload every tick.
    static std::atomic<uint64_t> s_uploadCount{0};
    static std::atomic<uint64_t> s_uploadLastReport{0};
    static std::atomic<uint64_t> s_frameMarker{0};
    using namespace std::chrono;
    const auto t0 = ayt::io::env::get("AY_RENDERER_UPLOAD_TIMING").has_value()
                        ? high_resolution_clock::now() : high_resolution_clock::time_point{};
    s_uploadCount.fetch_add(1, std::memory_order_relaxed);
    const uint64_t callIdx = s_uploadCount.load(std::memory_order_relaxed);

    VertexLayoutDesc layout;
    bgfx::VertexLayout bgfxLayout;
    if (!buildBgfxVertexLayoutFromMesh(mesh, layout, bgfxLayout)) {
        std::fprintf(stderr,
                     "[RenderAssetBridge] buildBgfxVertexLayoutFromMesh failed (mask=0x%02X stride=%u)\n",
                     mesh.getAttributeMask(), mesh.getVertexStride());
        return {};
    }

    if (mesh.getVertexCount() == 0 || mesh.getIndexCount() == 0
        || mesh.getVertexData() == nullptr || mesh.getIndexData() == nullptr) {
        std::fprintf(stderr, "[RenderAssetBridge] mesh data missing (v=%u i=%u)\n",
                     mesh.getVertexCount(), mesh.getIndexCount());
        return {};
    }

    const uint16_t meshPosOffset = mesh.hasAttribute(MeshAttribute::Position)
        ? mesh.getAttributeInfo(MeshAttribute::Position).offset : 0u;
    const uint16_t meshNrmOffset = mesh.hasAttribute(MeshAttribute::Normal)
        ? mesh.getAttributeInfo(MeshAttribute::Normal).offset : 0u;
    const uint16_t meshUvOffset = mesh.hasAttribute(MeshAttribute::UV)
        ? mesh.getAttributeInfo(MeshAttribute::UV).offset : 0u;
    const uint16_t bgfxPosOffset = bgfxLayout.getOffset(bgfx::Attrib::Position);
    const uint16_t bgfxNrmOffset = bgfxLayout.getOffset(bgfx::Attrib::Normal);
    const uint16_t bgfxUvOffset  = bgfxLayout.getOffset(bgfx::Attrib::TexCoord0);
    const uint16_t bgfxBiOffset  = bgfxLayout.has(bgfx::Attrib::Indices)
        ? bgfxLayout.getOffset(bgfx::Attrib::Indices) : 0u;
    const uint16_t bgfxBwOffset  = bgfxLayout.has(bgfx::Attrib::Weight)
        ? bgfxLayout.getOffset(bgfx::Attrib::Weight) : 0u;

    // Phase 0 RD-02: skin flag is the IMesh contract, not the layout. Pass it down
    // so RenderResourceManager records it on the resulting GpuMesh.
    const bool meshHasSkin = mesh.hasAttribute(MeshAttribute::SkinWeight)
        && mesh.hasSkinWeights()
        && mesh.getSkinWeights() != nullptr;

    std::fprintf(stderr,
                 "[RenderAssetBridge] mesh upload layout meshStride=%u bgfxStride=%u "
                 "pos=%u/%u nrm=%u/%u uv=%u/%u bi=%u bw=%u skin=%d\n",
                 mesh.getVertexStride(), bgfxLayout.getStride(),
                 meshPosOffset, bgfxPosOffset, meshNrmOffset, bgfxNrmOffset,
                 meshUvOffset, bgfxUvOffset, bgfxBiOffset, bgfxBwOffset,
                 meshHasSkin ? 1 : 0);

    std::vector<uint8_t> repacked;
    if (!repackMeshVertices(mesh, bgfxLayout, repacked)) {
        std::fprintf(stderr,
                     "[RenderAssetBridge] repack failed (src=%u dst=%u verts=%u)\n",
                     mesh.getVertexStride(), bgfxLayout.getStride(), mesh.getVertexCount());
        return {};
    }

    const void* vertexData = repacked.data();
    const uint32_t vertexStride = bgfxLayout.getStride();

    const MeshHandle handle = mgr.createMeshFromResourceData(vertexData,
                                          mesh.getVertexCount(),
                                          vertexStride,
                                          layout,
                                          mesh.getIndexData(),
                                          mesh.getIndexCount(),
                                          meshHasSkin);

    // Diag closure: print once per ~256 calls so a runaway
    // upload is loud but a stable scene is silent.
    if (ayt::io::env::get("AY_RENDERER_UPLOAD_TIMING").has_value()) {
        const auto t1 = high_resolution_clock::now();
        const double ms = duration<double, std::milli>(t1 - t0).count();
        std::fprintf(stderr,
                     "[RenderAssetBridge diag] upload #%llu took %.2fms "
                     "(verts=%u indices=%u meshStride=%u)\n",
                     static_cast<unsigned long long>(callIdx),
                     ms,
                     mesh.getVertexCount(), mesh.getIndexCount(),
                     mesh.getVertexStride());
    }
    return handle;
}

MaterialHandle bindMaterialFromResource(RenderResourceManager& mgr,
                                        const ayt::resource::IMaterial& material,
                                        const std::string& materialPath)
{
    const char* shaderRef = material.getShader();
    if (shaderRef == nullptr || shaderRef[0] == '\0') {
        std::fprintf(stderr, "[RenderAssetBridge] material has empty shader path '%s'\n",
                     materialPath.c_str());
        return {};
    }

    const std::string shaderPath =
        resolveShaderPath(materialPath, shaderRef);

    // Default: Phoskia file path. Force hand .sc with AY_SHADOW_USE_SC=1
    // (PowerShell: $env:AY_SHADOW_USE_SC="1").
    MaterialHandle handle{};
    const std::string useScEnv = ayt::io::env::get("AY_SHADOW_USE_SC").value_or("");
    const bool forceSc =
        !useScEnv.empty()
        && useScEnv[0] != '0';
    // Legacy: AY_SHADOW_USE_PHOSKIA=0 also forces .sc (compat with old docs).
    const std::string usePhoskiaEnv = ayt::io::env::get("AY_SHADOW_USE_PHOSKIA").value_or("");
    const bool forcePhoskiaOff =
        usePhoskiaEnv == "0";
    const bool preferSc =
        (std::strstr(shaderRef, "simple_lit_shadow") != nullptr)
        && (forceSc || forcePhoskiaOff);
    {
        static bool s_loggedOnce = false;
        if (!s_loggedOnce && std::strstr(shaderRef, "simple_lit_shadow") != nullptr) {
            std::fprintf(stderr,
                         "[RenderAssetBridge] simple_lit_shadow path=%s "
                         "(AY_SHADOW_USE_SC=%s AY_SHADOW_USE_PHOSKIA=%s)\n",
                         preferSc ? "bgfx .sc" : "Phoskia file",
                         useScEnv.empty() ? "(unset)" : useScEnv.c_str(),
                         usePhoskiaEnv.empty() ? "(unset)" : usePhoskiaEnv.c_str());
            s_loggedOnce = true;
        }
    }
    if (preferSc) {
        handle = mgr.createMaterialFromBgfxSc(
            ayt::render::kSimpleLitShadowVertexSc,
            ayt::render::kSimpleLitShadowFragmentSc,
            ayt::render::kSimpleLitShadowVaryingSc,
            ayt::render::kSimpleLitShadowScCacheKey);
        if (handle.isValid()) {
            std::fprintf(stderr,
                         "[RenderAssetBridge] simple_lit_shadow via bgfx .sc "
                         "(cacheKey=%s matId=%llu path=%s)\n",
                         ayt::render::kSimpleLitShadowScCacheKey,
                         static_cast<unsigned long long>(handle.id),
                         materialPath.c_str());
        }
    }
    if (!handle.isValid()) {
        handle = mgr.createMaterialFromFile(shaderPath);
        if (handle.isValid() && !preferSc) {
            std::fprintf(stderr,
                         "[RenderAssetBridge] simple_lit_shadow via Phoskia "
                         "file='%s' matId=%llu\n",
                         shaderPath.c_str(),
                         static_cast<unsigned long long>(handle.id));
        }
    }
    // Cached FBX aymats often reference missing shaders/pbr.phoskia.
    // Fall back to the editor-shipped lit shader so albedo textures still bind.
    if (!handle.isValid()) {
        const std::string fallback =
            resolveShaderPath(materialPath, "simple_lit_shadow.phoskia");
        handle = mgr.createMaterialFromFile(fallback);
        if (handle.isValid()) {
            std::fprintf(stderr,
                         "[RenderAssetBridge] shader '%s' missing; "
                         "fallback '%s' matId=%llu\n",
                         shaderPath.c_str(), fallback.c_str(),
                         static_cast<unsigned long long>(handle.id));
        }
    }
    if (!handle.isValid()) {
        std::fprintf(stderr,
                     "[RenderAssetBridge] createMaterial failed (mat='%s' shader='%s')\n",
                     materialPath.c_str(), shaderPath.c_str());
        return {};
    }

    if (const auto* concrete = dynamic_cast<const ayt::resource::Material*>(&material)) {
        concrete->forEachParameter([&](const char* name, MaterialParamType type) {
            applyMaterialParameter(mgr, material, materialPath, handle, name, type);
        });
    }

    if (!material.hasParameter("baseColor")) {
        mgr.setMaterialColor(handle, "baseColor", 1.0f, 1.0f, 1.0f, 1.0f);
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
