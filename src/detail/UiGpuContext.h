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
    // U1++ — kDefaultViewId = 1 removed (grep confirmed zero callers
    // in AYRuntime/AYRenderer/** and AliyatRenderer/**; UI has always
    // used UIRenderBackend::kViewId = 2 — view 1 is the 3D scene pass).
    // Historical artifact from an earlier "UI on view 1" draft that
    // never landed. kInvalidIdx stays — backs _whiteTexture.
    static constexpr uint16_t kInvalidIdx    = UINT16_MAX;

    bool initialize(ayt::shader::ShaderResourcePool& shaderPool, BGFXAdapter& adapter);
    void shutdown(ayt::shader::ShaderResourcePool& shaderPool, BGFXAdapter& adapter);
    bool isInitialized() const { return _initialized; }

    void beginView(uint8_t viewId, uint16_t width, uint16_t height);

    // P1: callers pass the full bgfx state (write flags + blend func).
    // State is per-run (item.state), so a BlendMode change starts a new run.
    void submitColoredQuads(uint8_t viewId, BGFXAdapter& adapter, uint64_t state,
                            const void* vertices, uint32_t vertexCount, uint32_t vertexStride,
                            const uint32_t* indices, uint32_t indexCount);

    void submitTexturedQuads(uint8_t viewId, BGFXAdapter& adapter, uint64_t state,
                             uint16_t textureIdx, const void* vertices, uint32_t vertexCount,
                             uint32_t vertexStride, const uint32_t* indices, uint32_t indexCount);

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
