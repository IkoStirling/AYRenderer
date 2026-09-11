#pragma once

#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResource.h"

#include <bgfx/bgfx.h>

#include <memory>
#include <vector>

namespace ayt::resource {
struct MeshMorphContract;
}

namespace ayt::render::detail
{

struct GpuMesh {
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  indexBuffer  = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  wireframeIndexBuffer = BGFX_INVALID_HANDLE;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t wireframeIndexCount = 0;
    // Phase 0 RD-02: true when this GpuMesh's vertex layout includes BoneIndices /
    // BoneWeights channels and the IMesh supplied non-null skin weights. Phase 1's
    // SkinnedForwardPass (RD-05) uses this flag to bind bone matrices.
    bool     hasSkinWeights = false;

    // Runtime-owned morph contract and mutable weights. The contract is shared
    // because a mesh cache entry may be inspected by animation/render systems
    // while reload replaces the owning GPU object.
    bool     hasMorphTargets = false;
    uint32_t morphTargetCount = 0;
    uint32_t morphDeltaCount = 0;
    uint8_t  morphPayloadChannels = 0;
    std::shared_ptr<const ayt::resource::MeshMorphContract> morphContract;
    std::vector<float> morphWeights;

    VertexLayoutDesc layout{};
    // CPU-side bind-pose bounds captured before upload. Shadow scene fitting
    // must not assume every mesh is a centered unit cube (tilemap chunks use
    // authored local coordinates and imported meshes have arbitrary extents).
    ayt::math::FVector3 localBoundsMin{-0.5f, -0.5f, -0.5f};
    ayt::math::FVector3 localBoundsMax{ 0.5f,  0.5f,  0.5f};
    bool localBoundsValid = false;
};

struct GpuTexture {
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    uint16_t width  = 0;
    uint16_t height = 0;
    // True when created via createDynamicTextureRgba8 (mem=nullptr) so
    // updateTextureFromRgba8 may rewrite pixels each frame (AYVideo V3).
    bool dynamic = false;
    // Sampling transfer function is part of the GPU resource identity.
    // Same file + different material semantic must not alias in the cache.
    bool srgb = false;
};

struct GpuMaterial {
    ayt::shader::ShaderResource shader;
    // Non-empty when the shader was loaded from disk; enables hot-reload refresh.
    std::string                 shaderSourcePath;
    ayt::shader::BindingId      colorBinding = ayt::shader::InvalidBinding;
    ayt::math::FVector4         colorOverride{1.0f, 1.0f, 1.0f, 1.0f};
    bool                        hasColorOverride = false;

    ayt::shader::BindingId      mat4Binding = ayt::shader::InvalidBinding;
    ayt::math::Float4x4         mat4Override = ayt::math::Float4x4::identity();
    bool                        hasMat4Override = false;

    // Phase 1 RD-04: cached UBO binding for the SkinnedLit's
    // `Skeleton` uniform block (mat4 bones[128]). ForwardOpaquePass
    // resolves this on first skinned draw and writes the per-frame
    // bone matrices via setUniformBlock on each subsequent draw.
    // InvalidBinding means "this material doesn't have a Skeleton block".
    ayt::shader::BindingId      boneBlockBinding = ayt::shader::InvalidBinding;

    // U1 — material-level blend state. ForwardOpaquePass ignores this
    // (it always draws opaque); TransparentPass::execute filters
    // scene.items() by transparent blend mode. Default = Opaque so
    // pre-existing materials (all created before this PR) keep their
    // prior no-blend draw path with no behavior change.
    ayt::render::BlendMode      blendMode = ayt::render::BlendMode::Opaque;
    ayt::render::MaterialModel  materialModel =
        ayt::render::MaterialModel::StandardLit;

    struct UniformSlot {
        std::string          name;
        ayt::shader::BindingId binding = ayt::shader::InvalidBinding;
        uint8_t              data[64]{};
        uint16_t             size = 0;
    };
    std::vector<UniformSlot> uniformSlots;

    struct TextureSlot {
        std::string          name;
        ayt::shader::BindingId binding = ayt::shader::InvalidBinding;
        TextureHandle        texture{};
    };
    std::vector<TextureSlot> textures;

    // Keep new surface-routing fields at the end of the internal record.
    // This preserves every pre-existing member offset if a localized MSVC
    // /showIncludes failure leaves a stale object in an incremental build.
    bool  alphaCutout = false;
    float alphaCutoff = 0.5f;
    bool  doubleSided = false;
    bool  premultipliedAlpha = false;
};

inline ayt::shader::TextureHandle toShaderTexture(bgfx::TextureHandle h)
{
    ayt::shader::TextureHandle out;
    if (bgfx::isValid(h)) {
        // bgfx handle idx can be 0; shader::TextureHandle uses id==0 as invalid.
        out.id = static_cast<uint64_t>(h.idx) + 1u;
    }
    return out;
}

} // namespace ayt::render::detail
