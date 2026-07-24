#pragma once

// §A1 SSAO MVP (2026-07-24, mid-term FG MVP SSAO Gate) — SSAOPass
// skeleton. Same SHIP scope as DepthHazePass S4a: declares the class
// shape (view id, cache-key extern, BindingId field matrix, lifecycle
// helpers), reserves the RenderPassSlot + FgResourceId +
// FgSemantic enum values, and pins the FG resolves-invalid ⇒ 0 draw
// contract. Real shader + noise texture + 8-tap worldPos sphere land
// in §A3. Default enabled = false / strength = 0 ⇒ zero alloc under
// the K-SSAO-1 invariant (frame.ssaoEnabled=false ⇒ SSAOTexture
// not live ⇒ resolve() returns invalid ⇒ pass early-returns 0).
//
// Pipeline position:
//   ... BloomExtract → BloomBlur → DepthHaze → SSAO → PostProcess → UI
//
// View id allocation (cutsheet §S2 lock):
//   BloomExtract=10 → BlurH=11 → BlurV=12 → DepthHaze=13 → SSAO=14
//   → PostProcess=15 → UI=255.
// SSAO sorts BEFORE Final PP (bgfx ascending view id) so the
// full-res occlusion RT is filled in the same frame PP samples it.
//
// Phoskia notes (lessons §3.1): all scalars as `uniform vec4` with
// .x carry. Phoskia has no `saturate` builtin → composite gate uses
// `clamp(1.0 - x, 0.0, 1.0)`. Sky reject (worldPos.w == 0) uses
// `step(0.0001, w)` not `if` (Phoskia has no if/else expression).
//
// Lifetime model:
//   - program + BindingId fields are private to this file. Acquire
//     happens lazily on first execute() and on cache-key bump.
//   - _noiseTex lazy-upload on first execute(). RGBA8 4×4 = 64 bytes
//     generated procedurally (tangent-rotation look-up). Destroyed
//     by destroyResources(); never enters FG (per cutsheet red line
//     #6: SSAO-owned internal resources stay with the pass).
//   - SSAOTexture RT is FG-owned (lazy create-on-first-resolve;
//     released by FrameGraph::resize / ::shutdown).
//
// K-SSAO invariants (must survive A2 pipeline wire + A3 composite):
//   1. ssaoEnabled=false OR ssaoStrength<=0 OR gbufferPass==nullptr
//      ⇒ FrameGraph compile culls SSAOTexture ⇒ resolve returns
//      invalid ⇒ SSAOPass::execute early-returns 0 (0 draw, 0 alloc).
//      PostProcessPass composite gate then binds sceneColor on the
//      SSAO sampler slot (F5-pattern fallback) and the FS
//      branchless `step`/`clamp` collapses the contribution to 0.
//   2. SSAOTexture deferred-only: `ssaoPassEnabled` at render() central
//      also gates on `gbufferPass != nullptr` so Forward pipeline
//      never sees the SSAOTexture resource. Matches cutsheet §S2
//      "Deferred-only" hard line.
//   3. ABI: append-only — RenderPassSlot::SSAO = 11 (the value 11
//      was unused in the post-S4 slot table). View ids:
//      SSAO = 14. FG does NOT allocate view ids.

#include "AYShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class SSAOPass : public RenderPass {
public:
    // §A1 SSAO view-map lock: BloomExtract=10 → BlurH=11 → BlurV=12
    // → DepthHaze=13 → SSAO=14 → PostProcess=15 → UI=255.
    // SSAO before Final PP so same-frame sampling works.
    static constexpr uint8_t kSsaoViewId = 14;

    SSAOPass() = default;
    // dtor does NOT touch bgfx handles. BGFXAdapter::shutdown()
    // invalidates globally. For mid-frame teardown, call
    // destroyResources() explicitly first. Mirror BloomExtractPass +
    // DepthHazePass lifetime contract.
    ~SSAOPass() override = default;

    std::string_view name() const override { return "SSAO"; }

    uint32_t execute(PassExecContext& ctx) override;

    // §A3/v4 — ready when Phoskia program acquired (noise optional; v4
    // uses a fixed kernel). Not virtual on the base class.
    bool isReady() const noexcept {
        return _program.isValid();
    }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). A1 ships with no real GPU work yet:
    // destroyResources is a no-op stub. A3 will fill in
    // VB/IB + program + noise-texture release + binding resets.
    void destroyResources(BGFXAdapter& adapter);

private:
    // ─── A1 skeleton state (mirrors DepthHazePass.h:138-170) ────
    // A3 fills in the program body + the noise-texture fields. The
    // BindingId list + signature order is reserved here so tests can
    // pin the field matrix without waiting for A3.
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;

    // Phoskia program — lazy-acquired on first execute() after
    // adapter init. Acquire may fail (shaderc missing on CI /
    // disk cache miss + parse error); in that case isReady() stays
    // false and execute() degrades to "early-return 0" (FG gate
    // already filtered the SSAOTexture resource out when
    // ssaoPassEnabled == false ⇒ resolve returns invalid ⇒ 0).
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on first acquire; InvalidBinding
    // means "not yet resolved / acquire failed".
    ayt::shader::BindingId      _uSSAOStrength    = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uSSAORadius      = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uSSAOBias        = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uCamPos          = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uViewportTexel   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tWorldPosition   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tWorldNormal     = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tNoise           = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every frame.
    bool                        _programAcquireFailed = false;

    // A3 will populate this (4×4 RGBA8 tangent-rotation noise).
    // A1 ships it as INVALID so destroyResources() is a clean no-op.
    bgfx::TextureHandle         _noiseTex          = BGFX_INVALID_HANDLE;
    bool                        _noiseUploaded      = false;

    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
    // A3 helper — A1 ships with a body that's a no-op so the field
    // above can be tested without needing the shader to compile.
    void ensureNoise(BGFXAdapter& adapter);
};

// §A1 (2026-07-24) — Bug-fix-#3 mirror (same pattern as
// kDepthHazeCacheKeyCStr in DepthHazePass.h). Externalize the
// cache-key literal so unit tests can include this header and
// compare their mirror against the live literal. Pre-A3, the
// literal is a placeholder string; A3 bumps it to the real SSAO
// shader cache key. The extern declaration gives every test a
// single source of truth; drift = test fails immediately.
extern const char* const kSSAOCacheKeyCStr;

} // namespace ayt::render::detail
