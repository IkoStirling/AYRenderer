#pragma once

// GBuffer attachment visualizer. When enabled, view 250 draws a fullscreen
// backbuffer overlay after the normal frame passes and before editor UI.
// Disabled mode remains zero-allocation and zero-draw.
//
// Pipeline position:
//   ... → GBuffer → Lighting → ... → SSAO → PostProcess → UI →
//     GBufferDebug(view 250) → Editor UI(view 255).
//
// View id allocation:
//   0=FO, 1=ShadowC, 2=ShadowR, 3=Trans, 4=PP-Fwd, 5=BloomExtract,
//   6=Skybox, 7=GBuffer, 8=Lighting, 9=Trans-Def, 10=PP-Def,
//   11=UI, 12=BloomBlurH, 13=BloomBlurV, 14=DepthHaze, 15=SSAO,
//   250=GBufferDebug, 255=UI-Editor.
//
// Contracts:
//   1. Disabled, uninitialized, Noop, or missing/currently-unproduced
//      GBuffer means an immediate zero-draw return.
//   2. Channel selection is branchless in the fragment shader.
//   3. Channel 3 visualizes the packed metallic/roughness/material-AO
//      scalars. Motion is retained only as a source-compatible enum alias;
//      no velocity attachment exists yet.
//   4. RenderPassSlot::GBufferDebug remains append-only at value 12.
//
// GPU objects (fullscreen triangle and program) are created lazily by the
// pass. The pass borrows the GBuffer textures and renders directly to the
// backbuffer; it owns no framebuffer or attachment.

#include "AYShader/ShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Six logical visualizations. Material decodes AO from RT2.a while the model
// view shows the independently packed shading-model ID. Motion remains a
// compatibility alias.
enum class GBufferDebugChannel : uint8_t {
    Albedo   = 0,  // RT0.rgb
    Normal   = 1,  // RT1.xyz, already encoded to [0,1]
    WorldPos = 2,  // RT2.xyz, remapped for display
    Material = 3,  // RGB = metallic / roughness / material AO
    Motion   = Material, // compatibility alias; no velocity RT exists yet
    Depth    = 4,  // depth attachment, reversed to near=white
    MaterialModel = 5, // StandardLit=green, Unlit=red
    Count    = 6,
};

class GBufferDebugPass : public RenderPass {
public:
    // Sits below 255=UI-Editor and above the 0..15 main-frame stream.
    static constexpr uint8_t kGBufferDebugViewId = 250;

    // Test/ABI mirror of the enum count.
    static constexpr uint8_t kGBufferDebugChannelCount =
        static_cast<uint8_t>(GBufferDebugChannel::Count);

    GBufferDebugPass() = default;
    // dtor does NOT touch bgfx handles. BGFXAdapter::shutdown()
    // invalidates globally. For mid-frame teardown, call
    // destroyResources() explicitly first. Mirror SSAOPass + DepthHaze
    // lifetime contract.
    ~GBufferDebugPass() override = default;

    std::string_view name() const override { return "GBufferDebug"; }

    uint32_t execute(PassExecContext& ctx) override;

    bool isReady() const noexcept {
        return _program.isValid();
    }

    // Call before adapter shutdown; releases the pass-owned triangle/program.
    void destroyResources(BGFXAdapter& adapter);

private:
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;

    // Lazy-acquired on first enabled execute after adapter initialization.
    ayt::shader::ShaderResource _program;

    // Cached bindings; InvalidBinding means unresolved/acquire failed.
    ayt::shader::BindingId      _uDebugChannel   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tAlbedo         = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tNormal         = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tWorldPos       = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tMaterial       = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tDepth          = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every frame.
    bool                        _programAcquireFailed = false;

    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

// External cache-key mirror used by the contract tests.
extern const char* const kGBufferDebugCacheKeyCStr;
extern const char* const kGBufferDebugPhoskiaSourceCStr;

} // namespace ayt::render::detail
