#pragma once
// AYRenderer.h — engine renderer entry (pimpl; no bgfx in this header)

#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include "aymath/MathTypes.h"
#include "aymath/MathUtils.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ayt::shader {
class ShaderResourcePool;
}

namespace ayt::render
{

class UIRenderBackend;

namespace detail {
class BGFXAdapter;
}

// High-level renderer: frame loop, resource handles, draw submission.
// GPU backend details live in src/detail/ only.
class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    bool initialize(const InitDesc& desc);
    void shutdown();
    bool isInitialized() const noexcept;

    void beginFrame(const ClearDesc& clear = {});
    // Full-window clear on view 0; subsequent render() draws 3D on view 1 with CLEAR_NONE.
    // Use with UIRenderBackend (view 2) so chrome pixels are not left uncleared.
    void beginCompositeFrame(const ClearDesc& clear, uint16_t fbWidth, uint16_t fbHeight);
    void render(const RenderScene& scene);
    void endFrame();

    void resize(uint32_t width, uint32_t height);
    void setViewportRect(uint16_t x, uint16_t y, uint16_t width, uint16_t height);

    MeshHandle createMesh(const void* vertices,
                          uint32_t vertexCount,
                          const VertexLayoutDesc& layout,
                          const uint16_t* indices,
                          uint32_t indexCount);
    MeshHandle createMesh32(const void* vertices,
                            uint32_t vertexCount,
                            const VertexLayoutDesc& layout,
                            const uint32_t* indices,
                            uint32_t indexCount);
    MeshHandle loadMesh(const std::string& path);
    MeshHandle createUnitCube();
    MeshHandle createTexturedUnitCube();

    MaterialHandle createMaterialFromPhoskia(const std::string& source,
                                             const std::string& cacheKey = "");
    MaterialHandle createMaterialFromBgfxSc(const std::string& vertexSc,
                                            const std::string& fragmentSc,
                                            const std::string& varyingDefSc,
                                            const std::string& cacheKey = "");
    MaterialHandle createMaterialFromFile(const std::string& path);
    MaterialHandle loadMaterial(const std::string& path);

    TextureHandle createTextureFromRgba8(uint32_t width, uint32_t height,
                                         const uint8_t* pixels,
                                         const std::string& cacheKey = "");
    TextureHandle createTextureFromFile(const std::string& path,
                                        const std::string& cacheKey = "");
    TextureHandle loadTexture(const std::string& path);

    void setMaterialColor(MaterialHandle material, const char* propertyName,
                          float r, float g, float b, float a = 1.0f);

    void setMaterialFloat(MaterialHandle material, const char* uniformName, float value);
    void setMaterialVec3(MaterialHandle material, const char* uniformName,
                         float x, float y, float z);

    void setMaterialMatrix4(MaterialHandle material, const char* uniformName,
                            const ayt::math::Float4x4& matrix);

    void setMaterialTexture(MaterialHandle material, const char* textureBindingName,
                            TextureHandle texture);

    void setMainCamera(const ayt::math::Float4x4& view,
                       const ayt::math::Float4x4& projection);

    // Build view/proj with bgfx/bx conventions (homogeneous depth, left-handed).
    void setMainCameraLookAtPerspective(const ayt::math::FVector3& eye,
                                        const ayt::math::FVector3& at,
                                        const ayt::math::FVector3& up,
                                        float fovYDegrees,
                                        float aspect,
                                        float nearZ,
                                        float farZ);

    void setDirectionalLight(const ayt::math::FVector3& direction,
                             const ayt::math::FVector3& color);

    void destroyMesh(MeshHandle& mesh);
    void destroyMaterial(MaterialHandle& material);
    void destroyTexture(TextureHandle& texture);

    void pollShaderHotReload();

    void setDebugOverlayEnabled(bool enabled);
    bool isDebugOverlayEnabled() const noexcept;
    void setDebugOverlaySuppressed(bool suppressed);
    bool isDebugOverlaySuppressed() const noexcept;
    void resetDebugOverlayStats();
    const RenderFrameStats& getFrameStats() const noexcept;

    // Queue a backbuffer capture for this frame. Call after render(), before endFrame().
    // Writes {base}.tga via bgfx, then {base}.png on the main thread after the frame.
    bool captureScreenshot(const std::string& filePath);

    // RD-07: path-cache introspection for unit tests / diagnostics.
    // Returns the number of distinct (normalized) asset paths currently
    // cached by RenderResourceManager for meshes and materials. Lets
    // tests assert that repeated loadMesh(loadMaterial) calls do not
    // grow the cache (the per-frame 0 disk-I/O invariant). O(1).
    size_t meshCacheSize() const;
    size_t materialCacheSize() const;

    // Debug: dump generated vs/fs/varying.def.sc under dir (creates dir if missing).
    void setShaderIntermediateDumpDirectory(const std::string& dir);

    // Persistent compiled shader cache (ShaderResourcePool Tier-1/2).
    void setShaderCacheDirectory(const std::string& dir);

    bool initializeUiRenderBackend(UIRenderBackend& backend);
    void shutdownUiRenderBackend(UIRenderBackend& backend);

private:
    friend class UIRenderBackend;
    detail::BGFXAdapter* bgfxAdapter() noexcept;
    const detail::BGFXAdapter* bgfxAdapter() const noexcept;
    ayt::shader::ShaderResourcePool* shaderPool() noexcept;
    const ayt::shader::ShaderResourcePool* shaderPool() const noexcept;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::render
