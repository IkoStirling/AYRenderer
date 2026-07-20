#pragma once

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace ayt::render::detail
{

// U1.5 — shared per-material uniform-upload helpers. ForwardOpaquePass
// and TransparentPass both need to upload MVP / cameraPos / lightDir /
// lightColor with the same lazy-resolve + fallback semantics. Keeping
// the bodies byte-for-byte identical in one header is cheaper than
// risking drift between two anonymous-namespace copies. Inline in the
// header so neither Pass owns the definition; the body is small (no
// cost to inline). Caller passes a ShaderResource by reference, so the
// lazy-resolve writes back into ShaderResource's internal binding
// cache (consistent with ForwardOpaquePass behavior since Phase 1).
//
// Why free fns (not static methods): anonymous-namespace free fns in
// the original ForwardOpaquePass.cpp would need to be exposed at
// namespace scope for TransparentPass to call them. Inline header
// free fns in `detail::` get us that visibility without forcing a
// non-inline definition in a .cpp.
inline void trySetUniformVec3(shader::ShaderResource& shader, const char* name, const float* values)
{
    const shader::BindingId binding = shader.getUniformBinding(name);
    if (binding == shader::InvalidBinding || values == nullptr) {
        return;
    }
    const float padded[4] = {values[0], values[1], values[2], 0.0f};
    shader.setUniform(binding, padded, sizeof(padded));
}

inline void trySetUniformMat4(shader::ShaderResource& shader, const char* primaryName,
                              const char* fallbackName, const ayt::math::Float4x4& matrix)
{
    shader::BindingId binding = shader.getUniformBinding(primaryName);
    if (binding == shader::InvalidBinding && fallbackName != nullptr) {
        binding = shader.getUniformBinding(fallbackName);
    }
    if (binding == shader::InvalidBinding) {
        return;
    }
    shader.setUniform(binding, matrix.ptr(), sizeof(float) * 16);
}

// U0 (Phase 2 Pass scaffold) — abstract base for one rendering pass.
// One subclass = one logical draw on one bgfx view. The pipeline
// (implemented in U1+ at detail/RenderPipeline.{h,cpp}) calls
// execute() in registration order; today the single call site is
// Renderer::render via RenderPipeline::executeAll.
//
// Design ref: `design.md:431-471` (RenderPass class + RenderPipeline
// + kFullPipelineOrder default table).
//
// Why a base class NOW: design.md promises polymorphic passes but
// live code has zero of them — ForwardOpaquePass is a free-standing
// concrete class with a single execute() called directly by
// Renderer::render. Adding Transparent / PostProcess / Shadow / UIPass
// today means duplicating dispatch wiring in Renderer::render for each
// one. A 55-line abstract base eliminates that; every subclass then
// fits into one registry vector (deferred to RenderPipeline in U1+).
//
// Non-goals in U0:
//   - No RenderPipeline / PassManager / FrameGraph (U1+).
//   - No PipelineState / RenderTargetHandle abstraction (bgfx owns RTs;
//     public headers must not include <bgfx/bgfx.h>; see Test_PublicHeaderSurface).
//   - Subclasses own their viewId + viewportRect internally via the
//     last two execute() args — no automatic sub-rect assignment.
//   - No `addToRegistry()` API on the base; that lives in U1+.
//
// Threading: execute() is called from the render thread (currently
// main thread; multiplayer deferred to Phase 5).
class RenderPass {
public:
    virtual ~RenderPass() = default;

    // Human-readable name for logs / debug overlay. Empty allowed.
    // Implementations return a static-lifetime string literal (no alloc).
    virtual std::string_view name() const = 0;

    // Render this pass into `viewId`. Returns draw-call count recorded
    // by the implementation (used to update RenderFrameStats).
    //
    // viewportX/Y/W/H describe the sub-rect of the framebuffer this
    // pass owns. For passes that draw over the full client (e.g. UI),
    // the host passes the full window rect.
    //
    // `materials` is non-const because some passes (ForwardOpaquePass)
    // lazily resolve BindingIds on first use and cache them in
    // GpuMaterial::colorBinding/etc. (See ForwardOpaquePass::flushMaterial
    // at ForwardOpaquePass.cpp:69-79.)
    //
    // pool is currently unused by the only concrete subclass
    // (ForwardOpaquePass) but is kept in the signature so future
    // passes (PostProcess / UIPass) that need shader-cache lookup
    // can reach the same ShaderResourcePool the Renderer owns.
    virtual uint32_t execute(
        BGFXAdapter& adapter,
        shader::ShaderResourcePool& pool,
        const RenderScene& scene,
        const std::unordered_map<uint64_t, GpuMesh>& meshes,
        const std::unordered_map<uint64_t, GpuTexture>& textures,
        std::unordered_map<uint64_t, GpuMaterial>& materials,
        uint16_t viewportX, uint16_t viewportY,
        uint16_t viewportWidth, uint16_t viewportHeight,
        const FrameContext& frame,
        uint8_t viewId) = 0;

    // Per-pass enable / disable (used by debug overlay toggle /
    // pipeline hot-reload in U1+). Default = enabled. Subclasses
    // inherit this state and never need to override.
    void setEnabled(bool enabled) { _enabled = enabled; }
    bool isEnabled() const { return _enabled; }

protected:
    // U1++ — shared color-uniform upload step. ForwardOpaquePass +
    // TransparentPass both need to (1) lazily resolve colorBinding
    // (baseColor -> color fallback), (2) re-validate the cached
    // binding each frame (hot-reload safety net), and (3) write
    // the override value or the neutral white default. Identity =
    // byte-for-byte between the two passes; lifted here so they
    // cannot diverge. Pure helper, no I/O beyond setUniform. Caller
    // must guard `material.shader.isValid()` first (both existing
    // call sites do).
    static void resolveAndApplyColorUniforms(GpuMaterial& material);

    bool _enabled = true;
};

} // namespace ayt::render::detail
