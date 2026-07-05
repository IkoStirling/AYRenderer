#pragma once

#include "AYShaderResource.h"

#include <cstdint>

namespace ayt::shader {
class ShaderResourcePool;
}

namespace ayt::render::detail {

class BGFXAdapter;

// Internal UI GPU resources via ShaderResourcePool (same shaderc path as engine).
class UiGpuContext {
public:
    static constexpr uint8_t  kDefaultViewId = 1;
    static constexpr uint16_t kInvalidIdx    = UINT16_MAX;

    bool initialize(ayt::shader::ShaderResourcePool& shaderPool, BGFXAdapter& adapter);
    void shutdown(ayt::shader::ShaderResourcePool& shaderPool, BGFXAdapter& adapter);
    bool isInitialized() const { return _initialized; }

    void beginView(uint8_t viewId, uint16_t width, uint16_t height);

    void submitColoredQuads(uint8_t viewId, BGFXAdapter& adapter,
                            const void* vertices, uint32_t vertexCount, uint32_t vertexStride,
                            const uint32_t* indices, uint32_t indexCount);

    void submitTexturedQuads(uint8_t viewId, BGFXAdapter& adapter, uint16_t textureIdx,
                             const void* vertices, uint32_t vertexCount, uint32_t vertexStride,
                             const uint32_t* indices, uint32_t indexCount);

    uint16_t whiteTextureIdx() const { return _whiteTexture; }

    uint16_t uploadTextTexture(BGFXAdapter& adapter, uint16_t width, uint16_t height,
                               const void* bgraPixels, uint32_t byteSize);

    void releaseTextTexture(BGFXAdapter& adapter, uint16_t textureIdx);

private:
    bool     _initialized  = false;
    uint16_t _whiteTexture = kInvalidIdx;

    ayt::shader::ShaderResource _shader;
    ayt::shader::BindingId      _texColorBinding = ayt::shader::InvalidBinding;
};

} // namespace ayt::render::detail
