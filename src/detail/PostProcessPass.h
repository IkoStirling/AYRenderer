#pragma once

#include "detail/RenderPass.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace ayt::render::detail
{

// P0 — R5+ deferred PostProcess scaffold (Phase P0, 2026-07-20).
//
// Why ship a no-op pass NOW: design.md:461 lists PostProcessPass in
// kFullPipelineOrder[5] ("Transparent", "PostProcess", "UI") but tags
// it "全屏 triangle + storage image（Phase 5 延后）". The pass slot
// has been empty since U1+ shipped [ForwardOpaque, Transparent, UI].
// materializing a real fullscreen-triangle + RT ping-pong requires:
//   1) A new bgfx::FrameBufferHandle ownership model on BGFXAdapter
//      (BGFXAdapter only owns VertexBuffer/IndexBuffer/TextureHandle
//      today — see BGFXAdapter.h:21-69).
//   2) A fullscreen-triangle Phoskia shader (needs shaderc in CI;
//      breaks the Noop-backend headless test path).
//   3) FrameContext extensions for time/bloomStrength/exposure (none
//      of these knobs exist yet — see FrameContext.h:8-14).
//
// Those three are R5+ deferred work. What P0 lands:
//   * The POLYMORPHIC SLOT between Transparent and UI — RenderPass
//     base dispatch now visits PostProcess in the correct order even
//     though it draws zero today.
//   * The ENABLE hook — `setEnabled(false)` lets hosts skip the slot
//     without ripping the pass out of the pipeline (matches
//     UIPass/ForwardOpaquePass per-pass isEnabled() contract from
//     U1+).
//   * The TIMING FIELD on FrameContext (`timeSeconds`) so a R5+
//     PostProcess implementation that drives shader uniforms per
//     frame does not need to extend the context later.
//   * The FUTURE-EXPANSION NOTES as a comment breadcrumb so R5+
//     contributors have the integration points mapped (see .cpp).
//
// Execute semantics (P0, locked):
//   1) Honor isEnabled() — default true; matches RenderPass base.
//   2) Skip work if no R5+ setup has been configured (no shader
//      has been injected; nothing to dispatch). Returns 0.
//   3) When R5+ lands: bind framebuffer N, run fullscreen triangle
//      with scene-color-as-texture input + authored shader, blit to
//      viewId. The execute() body in .cpp will grow at that point —
//      no signature change required.
//
// What execute() does NOT do (P0):
//   - It does NOT touch the adapter (no setViewRect /
//     setViewTransform / setViewClear) — bgfx's framebuffer model
//     needs a separate FBO abstraction we haven't built yet.
//   - It does NOT consume the FrameContext fields other than reading
//     them for future uniform upload (today the read is stubbed).
//   - It does NOT allocate any GPU resources — DeferredPostShader
//     (R5+) will own the fullscreen-triangle mesh + FBO.
class PostProcessPass : public RenderPass {
public:
    std::string_view name() const override { return "PostProcess"; }

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

    // P0 — query / toggle the future R5+ shader injection. R5+ will
    // extend this to a real ShaderResource owned by the pass; today
    // hasShader() == false is the documented no-op branch and
    // setShader(nullptr) is the documented reset. Public so the host
    // can introspect whether the pass is wired.
    bool hasShader() const noexcept { return false; }
    void setShader(void* /*placeholder*/) noexcept { /* R5+ */ }
};

} // namespace ayt::render::detail
