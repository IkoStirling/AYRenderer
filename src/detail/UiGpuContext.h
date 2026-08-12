#pragma once

#include "AYShaderResource.h"

#include <aymath/MathTypes.h>

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

    // P3: UI texture upload — same BGRA8 path as uploadTextTexture but with
    // linear filtering (no POINT bits) + clamp wrap, so stretched UI art
    // doesn't bleed edge texels across 9-patch seams. uploadTextTexture's
    // signature is untouched (font atlas path zero contact).
    uint16_t uploadUiTexture(BGFXAdapter& adapter, uint16_t width, uint16_t height,
                             const void* bgraPixels, uint32_t byteSize);

    void releaseTextTexture(BGFXAdapter& adapter, uint16_t textureIdx);

    // P2 — SDF rounded-rect primitive. Every uniform is a standalone
    // vec4 (bgfx uniforms do not support arrays — ShaderResourcePool's
    // tryInjectDeclaredUniforms stops at '['). Colors are straight
    // (non-premultiplied) RGBA; alpha 0 disables the effect. FVector4 /
    // FVector2 keep the style of the IRenderBackend interface and give
    // whole-value assignment (raw float[] members can't be assigned).
    struct SdfParams {
        // Batch knife: the shape rect is NOT here — it rides per-vertex
        // (UiVertex shapeCx/Cy/HalfW/HalfH) so identical-param SDF items
        // share one submission. Run key is sdfParamsEqual() (not memcmp —
        // FVector4 padding would false-break merges).
        ayt::math::FVector4 radius;        // rTL rTR rBR rBL (CPU-clamped)
        ayt::math::FVector4 strokeColor;   // a==0 → no stroke
        float strokeWidth = 0.0f;          // ring thickness, px
        float strokeInset = 0.0f;          // ring center offset; outside stroke = +w/2 (center lands on true rect edge)
        ayt::math::FVector4 shadowColor;   // a==0 → no shadow
        ayt::math::FVector2 shadowOffset;  // dx dy
        float shadowBlur  = 0.0f;          // radius expansion = soft shadow approx
    };

    // One quad per Sdf item (params ride along via uniforms). Vertices
    // use the shared UiVertex layout with the white texture bound;
    // TexCoord2 = soft-clip rect (center, half-extent; all-zero = none).
    void submitSdfQuads(uint8_t viewId, BGFXAdapter& adapter, uint64_t state,
                        const void* vertices, uint32_t vertexCount, uint32_t vertexStride,
                        const uint32_t* indices, uint32_t indexCount, const SdfParams& params);

private:
    bool     _initialized  = false;
    uint16_t _whiteTexture = kInvalidIdx;

    ayt::shader::ShaderResource _shader;
    ayt::shader::BindingId      _texColorBinding = ayt::shader::InvalidBinding;

    // P2: SDF program + its 5 uniform bindings (resolved at initialize;
    // any Invalid binding fails init so tests go red on shader drift).
    ayt::shader::ShaderResource _sdfShader;
    ayt::shader::BindingId      _sdfTexBinding    = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _sdfRadiusBinding = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _sdfStrokeBinding = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _sdfStrokeWidBind = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _sdfShadowBinding = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _sdfShadowOffBind = ayt::shader::InvalidBinding;
};

} // namespace ayt::render::detail
