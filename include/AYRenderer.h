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
    // §P5.5 D-upload — host-facing cubemap create (6×RGBA8 faces).
    TextureHandle createCubeTextureFromRgba8(uint32_t size,
                                             const uint8_t* rgba8Faces,
                                             const std::string& cacheKey = "");
    TextureHandle createTextureFromFile(const std::string& path,
                                        const std::string& cacheKey = "");
    TextureHandle loadTexture(const std::string& path);

    void setMaterialColor(MaterialHandle material, const char* propertyName,
                          float r, float g, float b, float a = 1.0f);

    // U1 — material-level blend mode. Default = Opaque for all
    // materials created before this PR (no behavior change). Set to
    // BlendMode::Alpha to make TransparentPass submit this material
    // with BGFX_STATE_BLEND_ALPHA in its second-pass slot. No-op on
    // unknown handle.
    void setMaterialBlendMode(MaterialHandle material, BlendMode blendMode);

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

    // Eye position last written by setMainCameraLookAtPerspective (default
    // until then). Used by RenderSystem for Transparent back-to-front sortKey.
    ayt::math::FVector3 mainCameraPosition() const noexcept;

    void setDirectionalLight(const ayt::math::FVector3& direction,
                             const ayt::math::FVector3& color);

    // §P5 B7+ (2026-07-22) — host-facing multi-light DataSource.
    // Drives the Deferred LightingPass's accumulation loop.
    // B5's single light via setDirectionalLight still works (the
    // renderer mirrors it into SceneLights::lights[0] as a fallback
    // when host has not called setSceneLights yet).
    //
    // Wiring contract: the renderer borrows the `lights` pointer
    // for the duration of one render() call. The SceneLights
    // instance must therefore outlive render(). Easiest lifetime
    // is a member of the host class (e.g. AppState / EditorApp),
    // populated by host game logic between render() calls.
    //
    // Passing a lights pointer with count == 0 silences the
    // multi-light upload (LightingPass falls back to the B5 single
    // light via FrameContext — see setDirectionalLight above).
    void setSceneLights(const SceneLights* lights);

    // §Skybox0 (2026-07-23) — host-facing Skybox DataSource.
    // Drives the Deferred SkyboxPass's equirect-panorama blit.
    // Default Forward pipeline (RenderPipelineDesc::makeDefault())
    // does NOT include the Skybox slot — Skybox is opt-in via
    // `configurePipeline(makeDeferred())`. Forward hosts that call
    // setSkySource() see 0 behavior change (Skybox slot absent in
    // the pipeline ⇒ SkyboxPass not instantiated ⇒ renderer
    // silently ignores the borrowed pointer).
    //
    // Lifetime contract: same shape as setSceneLights — the
    // SkySource instance must outlive any in-flight render() call.
    // Easiest lifetime = a host member, populated once at startup
    // or per scene change.
    //
    // Passing a nullptr (or an inactive SkySource — see
    // `SkySource::isActive()`) silences the sky blit. Hosts that
    // want a per-frame sky swap can mutate the SkySource in
    // place; the renderer reads the borrowed pointer each frame.
    void setSkySource(const SkySource* sky);
    const SkySource* skySource() const noexcept;

    // §P5.5 D (2026-07-23) — IBL MVP (Ambient Diffuse Cube Lookup).
    // Host-side upload setter for the cube map (samplerCube)
    // consumed by SkyboxPass::execute (kind=CubeMap path) AND
    // LightingPass::execute (ambient cube lookup term).
    //
    // The cube handle is a host-owned TextureHandle resource
    // (mirror equirect `texture2d` path semantics — same Texture
    // Resource lifetime; the renderer borrows the bgfx::TextureHandle
    // lookup via ctx.textures). Default = TextureHandle{} (invalid)
    // = cube path inactive = SkyboxPass kind stays Equirect (or
    // no-op if no equirect) and LightingPass uploads cubeActive=0
    // ⇒ pre-D byte-equivalent flat ambient + flat backdrop.
    //
    // Hard rule (owner-confirmed 2026-07-23): cube handle valid ⇒
    // CubeMap path; invalid (or setSkySourceCube never called) ⇒
    // fallback to the equirect setSkySource path. The two paths are
    // MUTUALLY EXCLUSIVE per frame — never "each draws half". The
    // SkyboxPass FS uses `mix(equirectColor, cubeColor, skyKind)`
    // with skyKind derived from `kind == CubeMap && hasValidCube`;
    // the LightingPass FS uses `ambientFlat + ambientCube *
    // cubeActive` with cubeActive derived from the same predicate.
    //
    // Pass an invalid TextureHandle to clear the cube path and
    // revert to equirect.
    //
    // Lifetime: the TextureHandle must outlive any in-flight
    // render() call (mirror equirect handle lifetime). Easiest
    // lifetime = a host member, populated once at startup or per
    // scene change. For hosts that want to dynamically swap cube
    // maps (e.g. room-to-room IBL transition), swap the handle
    // between render() calls — pass the new handle, then call
    // render().
    //
    // Host-side cube upload: Renderer::createCubeTextureFromRgba8
    // (6×RGBA8 faces). HDR / file-based cube load can follow later.
    void setSkySourceCube(TextureHandle cube);
    TextureHandle skySourceCube() const noexcept;

    // §P5.5 D — IBL ambient cube strength (LightingPass ambientStrength.x).
    // Default 0.6; Editor acceptance uses ~0.85 for visible env tint.
    void  setAmbientStrength(float strength);
    float ambientStrength() const noexcept;

    // Quality / style knobs (safe to call after initialize).
    // msaa: 0=off, 2/4/8/16. Applies via bgfx::reset (backbuffer).
    void setMsaaSampleCount(uint32_t samples);
    uint32_t msaaSampleCount() const noexcept;
    // Soft shadow PCF (3x3). false → hard 1-tap (stylized edges).
    void setShadowPcfEnabled(bool enabled);
    bool shadowPcfEnabled() const noexcept;

    // E5 (§5.4, 2026-07-22) — live read of the Shadow slot's enabled
    // flag in the current pipeline. Default ctor + configurePipeline(
    // makeDefault()) + configurePipeline(makeForwardWithShadows()) all
    // return true (Shadow enabled by default). Pass a custom desc that
    // omits RenderPassSlot::Shadow to opt out; returns false then.
    // Const noexcept; no mirror field — reads the pass directly via
    // findPass("Shadow").
    bool shadowsEnabled() const noexcept;

    // §P5 B3 (2026-07-22) — live read of the Lighting slot's enabled
    // flag in the current pipeline. Default ctor + configurePipeline(
    // makeDefault()) return false (no Lighting slot mounted on
    // Forward path). After configurePipeline(makeDeferred()), returns
    // true (Lighting slot enabled by RenderPass base default
    // _enabled == true). Pass a custom desc that omits
    // RenderPassSlot::Lighting to opt out; returns false then.
    // Const noexcept; no mirror field — reads the pass directly via
    // findPass("Lighting").
    bool lightingEnabled() const noexcept;

    // P4.2 (§P4, 2026-07-22) — global shadow receiver bias. Host
    // controls this in ndc01 units (same as the Phoskia receiver
    // shadowBias property — see AYShadowShaderSources.h:81 + the
    // simple_lit_shadow.phoskia receiver contract). The renderer
    // uploads this value into every receiver material's `shadowBias`
    // uniform via tryBindShadowSampler; per-material
    // setMaterialVec3(material, "shadowBias", v) still works for
    // overrides (the global value lands AFTER per-material writes
    // for that frame; future per-material override machinery can
    // gate that ordering).
    //
    // Typical values: 0.001–0.005. Default 0.003f matches the
    // Phoskia property default + ShadowSettings::kBiasDefault.
    // Set 0 to disable global bias (per-material still active).
    // Noop/headless: setter is a no-op until initialize(); getter
    // returns the stored value.
    void   setShadowBias(float bias);
    float  shadowBias() const noexcept;

    // R5+ (Phase PostProcess) — knobs → FrameContext each frame.
    // Defaults: bloom=0, exposure=1, gamma=2.2, tonemap=None.
    // Shader uniforms are vec4 slots (.x used); see
    // docs/pass-lessons-from-shadow.md §3.1.
    void setPostProcessBloomStrength(float strength);
    void setPostProcessExposure(float exposure);
    // Display gamma for final blit encode (pow(c, 1/gamma)). Default 2.2.
    void setPostProcessGamma(float gamma);
    // Freeze FrameContext.timeSeconds. When paused, render() keeps the
    // last sampled value so Editor Pause freezes time-driven PP knobs
    // without stopping the composite blit.
    void setPostProcessClockPaused(bool paused);
    bool isPostProcessClockPaused() const noexcept;
    // Drive FrameContext.timeSeconds from the host simulation clock
    // (typically GameLoop::getElapsedTime). When set, overrides the
    // wall-clock path. Call once per composite frame.
    void setSimulationTimeSeconds(float seconds);
    enum class TonemapMode : uint8_t {
        None     = 0,
        Reinhard = 1,
        ACES     = 2,
    };
    void setPostProcessTonemapMode(TonemapMode mode);

    // Rebuild the RenderPipeline from an ordered pass-slot list.
    // Default ctor builds makeDefault() which mounts Shadow at slot 0
    // *enabled* (E5 §5.4, 2026-07-22). To opt out, pass a custom desc
    // that omits RenderPassSlot::Shadow. Empty `passes` falls back to
    // makeDefault(). Preserves any UI backend previously injected via
    // setUiBackend. Live state via shadowsEnabled().
    void configurePipeline(const RenderPipelineDesc& desc);
    const RenderPipelineDesc& pipelineDesc() const noexcept;

    void destroyMesh(MeshHandle& mesh);
    void destroyMaterial(MaterialHandle& material);
    void destroyTexture(TextureHandle& texture);

    void pollShaderHotReload();

    // Re-compile materials already loaded from `shaderPath` (e.g. after
    // EditorPlayRuntime rewrites a .phoskia on disk). No-op when renderer
    // is not initialized or no matching materials are loaded yet.
    uint32_t reloadMaterialsForShaderFile(const std::string& shaderPath);

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

    // U1+ — inject a UIRenderBackend into the RenderPipeline's UIPass
    // so the renderer can dispatch UI draws from Renderer::render.
    // Replaces the AYEditorApp hand-roll that bypassed
    // initializeUiRenderBackend. Null is allowed (UIPass::execute
    // short-circuits to 0 draws). Pointer is non-owning; backend
    // lifetime is the host's responsibility.
    void setUiBackend(UIRenderBackend* backend);

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
