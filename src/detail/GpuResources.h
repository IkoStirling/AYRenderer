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
    shader::ShaderResource shader;
    shader::BindingId      colorBinding = shader::InvalidBinding;
    ayt::math::FVector4    colorOverride{1.0f, 1.0f, 1.0f, 1.0f};
    bool                   hasColorOverride = false;

    shader::BindingId      mat4Binding = shader::InvalidBinding;
    ayt::math::Float4x4    mat4Override = ayt::math::Float4x4::identity();
    bool                   hasMat4Override = false;

    struct UniformSlot {
        shader::BindingId binding = shader::InvalidBinding;
        uint8_t           data[64]{};
        uint16_t          size = 0;
    };
    std::vector<UniformSlot> uniformSlots;

    struct TextureSlot {
        shader::BindingId binding = shader::InvalidBinding;
        TextureHandle     texture{};
    };
    std::vector<TextureSlot> textures;
};

inline shader::TextureHandle toShaderTexture(bgfx::TextureHandle h)
{
    shader::TextureHandle out;
    if (bgfx::isValid(h)) {
        // bgfx handle idx can be 0; shader::TextureHandle uses id==0 as invalid.
        out.id = static_cast<uint64_t>(h.idx) + 1u;
    }
    return out;
}

} // namespace ayt::render::detail
