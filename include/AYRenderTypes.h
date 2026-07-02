#pragma once
// AYRenderTypes.h — public renderer types (no bgfx / no driver handles)

#include "AYMathTypes.h"

#include <cstdint>
#include <string>

namespace ayt::render
{

enum class Backend : uint8_t {
    Auto = 0,
    Direct3D11,
    Direct3D12,
    Vulkan,
    OpenGL,
    Metal,
    Noop,  // headless / unit tests only
};

struct InitDesc {
    void*    windowHandle = nullptr;
    uint32_t width        = 1280;
    uint32_t height       = 720;
    bool     vsync        = true;
    Backend  backend      = Backend::Auto;
    bool     enableDebugOverlay = false;
};

struct RenderFrameStats {
    float    fps = 0.0f;
    float    frameTimeMs = 0.0f;
    float    avgFrameTimeMs = 0.0f;
    uint32_t drawCalls = 0;
    uint32_t sceneItems = 0;
    uint64_t frameCount = 0;
};

struct ClearDesc {
    float r = 0.05f;
    float g = 0.05f;
    float b = 0.08f;
    float a = 1.0f;
    bool  clearDepth = true;
};

// Vertex layout description (OpenGL-style attributes, no bgfx types in public API).
enum class VertexAttribute : uint8_t {
    Position,
    Normal,
    TexCoord0,
    Tangent,
    Color0,
};

enum class VertexComponentType : uint8_t {
    Float,
    Uint8,
};

struct VertexElement {
    VertexAttribute     attribute = VertexAttribute::Position;
    uint8_t             componentCount = 3;
    VertexComponentType componentType = VertexComponentType::Float;
    bool                normalized = false;
};

struct VertexLayoutDesc {
    static constexpr uint8_t kMaxElements = 8;

    VertexElement elements[kMaxElements]{};
    uint8_t       elementCount = 0;

    bool add(const VertexElement& element);
    bool isValid() const noexcept;
    uint32_t strideBytes() const noexcept;

    // Common presets (logical attribute order; bgfx may add padding — repack on upload).
    static VertexLayoutDesc position3();
    static VertexLayoutDesc position3Normal3();
    static VertexLayoutDesc position3TexCoord2();
    static VertexLayoutDesc position3Normal3TexCoord2();
};

struct MeshHandle {
    uint64_t id = 0;
    bool isValid() const noexcept { return id != 0; }
};

struct MaterialHandle {
    uint64_t id = 0;
    bool isValid() const noexcept { return id != 0; }
};

struct TextureHandle {
    uint64_t id = 0;
    bool isValid() const noexcept { return id != 0; }
};

} // namespace ayt::render
