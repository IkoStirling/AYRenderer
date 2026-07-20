#pragma once

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace ayt::render::detail
{

// R5+ (Phase Shadow, 2026-07-20) — shadow caster pass slot per
// design.md §8.3. Default-disabled; a future PR will flip it to
// default-enabled once the deferred pipeline plumbing (Light
// struct in RenderScene + lighting consumer in ForwardOpaquePass
// for forward-lit materials) lands. Today the pass exists so:
//   1) Hosts can opt into shadow casting by addPass()ing a
//      ShadowPass in front of their forward pipeline.
//   2) Tests can pin that the slot wires through RenderPipeline
//      without disturbing the existing 4-pass default.
//   3) Future work (cascaded shadow, Point/Spot shadow caster
//      programs, scene-AABB fit) has a well-typed home.
//
// v1 behavior (this PR):
//   - Renders every DrawItem in scene.items() into a depth-only
//     FBO at the host-supplied shadow map size (default 1024).
//   - Uses bgfx's no-fragment-program submit (state = WRITE_Z |
//     DEPTH_TEST_LESS + DISCARD_ALL flag) to write depth without
//     fragment shader cost. Same trick every vanilla shadow-map
//     example uses.
//   - Auto-degrades to 0 draws on the Noop backend (mirrors
//     PostProcessPass::execute guard) so headless tests stay clean.
//
// Scope-shrink from full ShadowPass design (deferred):
//   - No Phoskia shadow_caster program (skin, alpha-clip, etc.)
//     — see design.md §8.3.4 cut 1 → cut 2 ordering.
//   - No cascade / cube shadow / shadow bias / PCF filtering.
//   - No scene-AABB fit — fixed 50-unit-radius frustum at origin.
//   - No Light struct on RenderScene (the missing piece that
//     caused the prior segfault). The pass picks the host-supplied
//     shadow map size only; the light-direction transform is the
//     default identity (so shadow writes are equivalent to depth
//     pre-pass; the visual signal will be in cut 2).
class ShadowPass : public RenderPass {
public:
    static constexpr uint16_t kDefaultShadowMapSize = 1024;

    ShadowPass() = default;
    ~ShadowPass() override;

    std::string_view name() const override { return "Shadow"; }

    uint32_t execute(
        BGFXAdapter& adapter,
        shader::ShaderResourcePool& pool,
        const RenderScene& scene,
        const std::unordered_map<uint64_t, GpuMesh>& meshes,
        const std::unordered_map<uint64_t, GpuTexture>& textures,
        std::unordered_map<uint64_t, GpuMaterial>& materials,
        uint16_t viewportX, uint16_t viewportY,
        uint16_t viewportWidth, uint16_t viewportHeight,
        const FrameContext& frame,
        uint8_t viewId) override;

    // R5+ — true when the pass has a live shadow FBO. Tests use
    // this to pin the slot behavior; hosts can also use it to skip
    // downstream shadow sampling when no shadow was rendered.
    bool isReady() const noexcept { return bgfx::isValid(_shadowFbo); }

    // R5+ — host can override the shadow map size before pipeline
    // dispatch (set before Renderer::render). Default = 1024.
    void setShadowMapSize(uint16_t size) noexcept { _requestedSize = size; }
    uint16_t shadowMapSize() const noexcept { return _requestedSize; }

    void destroyResources(BGFXAdapter& adapter);

private:
    void ensureShadowFbo(BGFXAdapter& adapter, uint16_t size);

    bgfx::FrameBufferHandle    _shadowFbo        = BGFX_INVALID_HANDLE;
    uint16_t                   _shadowSize       = 0;
    uint16_t                   _requestedSize    = kDefaultShadowMapSize;
};

} // namespace ayt::render::detail
