#pragma once

#include "AYRenderTypes.h"
#include "AYShaderResource.h"

#include <bgfx/bgfx.h>

#include <vector>

namespace ayt::render::detail
{

struct GpuMesh {
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  indexBuffer  = BGFX_INVALID_HANDLE;
    uint32_t indexCount = 0;
    VertexLayoutDesc layout{};
};

struct GpuTexture {
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    uint16_t width  = 0;
    uint16_t height = 0;
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
