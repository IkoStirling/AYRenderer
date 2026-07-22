#pragma once

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/BgfxMatrix.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>

// Forward declaration — tryBindShadowSampler (below) only needs a
// pointer; ShadowPass.h pulls in <bgfx/bgfx.h> and the full class
// definition which we don't want leaking into every TU that
// already includes RenderPass.h for the trySet helpers.
namespace ayt::render::detail { class ShadowPass; }

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
    float colMajor[16];
    toBgfxColumnMajor(matrix, colMajor);
    shader.setUniform(binding, colMajor, sizeof(colMajor));
}

// PR-F2 / Phase 5 — bind shadowMap + upload light-space VP for receivers.
// See AYShadowReceiverContract.h for the material contract.
//
// `flags` gates Receive: Cast-only / None items get the lit fallback so
// meshes without Receive stay fully lit even if the shader declares shadowMap.
//
// `shadowPass == nullptr` or missing FBO/sample → lit fallback (safe).
// Materials without `shadowMap` → no-op (pre-shadow shaders unchanged).
void tryBindShadowSampler(shader::ShaderResource& shader,
                          BGFXAdapter& adapter,
                          const ShadowPass* shadowPass,
                          ShadowFlags flags = kShadowCastAndReceive);

// PR-F3 (2026-07-21) — bone-palette upload shared by
// ForwardOpaquePass's draw loop AND ShadowPass's caster draw loop.
//
// Lifted from ForwardOpaquePass::execute's inline block (now byte-
// for-byte identical to that block's CPU behavior — only call site
// moved). The pass-shader may be the SkinnedLit (forward) or the
// depth-only Caster — both link a `Skeleton` UBO so the same bytes
// work.
//
// `castSkinned` arg is the uniform toggle for programs that
// distinguish static / skinned VS segments (the depth caster does
// via `property castSkinned`). ForwardOpaquePass passes the value
// it inherits from the GpuMaterial's lazily-resolved uniform (kept
// = 0 there — SkinnedLit's VS is unconditional); ShadowPass passes
// `item.boneMatrices != nullptr ? 1 : 0` so the caster program
// selects the right VS segment.
//
// `skeletonBinding` may be `shader::InvalidBinding` for non-
// skeletal programs — the helper no-ops cleanly (matching the
// pre-F3 "missing binding → log 3x, skip upload" behavior). When
// `castSkinned` arg selects is also nullified when
// `castSkinned == 0` to avoid a redundant uniform write.
//
// Stack-path for ≤ 16 joints (the common case — model files
// today cap joint counts well below the Skeleton UBO's 128-
// element range). Heap-path for larger counts preserves the pre-
// F3 allocation pattern.
//
// Pure: no other state touched beyond `shader.setUniformBlock` /
// `shader.setUniform`.
void tryUploadBonePalette(shader::ShaderResource& shader,
                          shader::BindingId skeletonBinding,
                          shader::BindingId castSkinnedBinding,
                          uint8_t castSkinnedValue,
                          const DrawItem& item);

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

    // Render this pass into the bgfx view + viewport described by
    // `ctx`. Returns draw-call count recorded by the implementation
    // (used to update RenderFrameStats).
    //
    // The 12-arg signature collapsed into `detail::PassExecContext` in
    // PR-C (P1, 2026-07-20). See PassExecContext.h for what each field
    // is and why. `frame` stays const per docs/execution-plan.md §5.3.
    //
    // `materials` inside the context is non-const because some passes
    // (ForwardOpaquePass) lazily resolve BindingIds on first use and
    // cache them in GpuMaterial::colorBinding/etc. (See
    // ForwardOpaquePass::flushMaterial.)
    virtual uint32_t execute(PassExecContext& ctx) = 0;

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
