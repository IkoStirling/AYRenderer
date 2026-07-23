#pragma once

// S1a BloomExtractPass (short-term-plan §S1, 2026-07-23) — first
// half-resolution effect pass in the pipeline. Inserts between
// TransparentPass and PostProcessPass. Reads the LIT scene color
// (B6 LightingOutputFbo on Deferred path, sceneFbo on Forward path,
// via PostProcessPass::selectSourceFbo helper) and writes a
// bright-thresholded version to a half-resolution RGBA8 FBO that
// the future S1b BloomBlurPass will sample.
//
// Why now: the short-term-plan §S1 cutsheet mandates a true
// half-resolution bloom chain (NOT the pre-S1 fake
// `raw + raw*bloomStrength` PostProcessPass shader hack). The
// cutsheet splits the work into 4 sub-cuts (S1a extract / S1b blur
// ping-pong / S1c Final PP compositing / S1d Editor knob). S1a is
// the wire + the extract shader; S1b/c are future cuts.
//
// View id allocation (cutsheet §S1 §1 + §5.3 spirit): composite view
// table 0..8 + UI=11 already taken by Shadow(1)/ShadowResolve(2)/
// FO(0)/Trans(3,9)/PP(4,10)/Skybox(6)/GBuffer(7)/Lighting(8)/UI(11).
// View 5 was the only unused slot before S1a; we claim it for
// BloomExtract. View id matches the cutsheet view-id table (lock
// per docs/execution-plan.md §5.1 — append-only).
//
// Lifecycle (cutsheet §S1 "FBO 生命周期：ensure(w/2, h/2)，跟 viewport
// resize"): half-resolution RGBA8 FBO with no depth attachment,
// create-once + resize-on-viewport-change, BGFXAdapter owns the
// underlying bgfx handle; this class owns the cache + destroy
// decision (mirror PostProcessPass::ensureFbo).
//
// Phoskia uniform gates (lessons §3.1): all scalar knobs uploaded
// as `uniform vec4` with .x carry — bgfx uniform slot is Vec4
// (4-byte float upload is UB), Phoskia `vec4 + .x` is the safe
// contract.
//
// Noop-backend safety: dual guard `!isInitialized() || isNoopBackend()`
// (mirror PostProcessPass + ShadowPass + GBufferPass + LightingPass +
// SkyboxPass). When either guard fires, the entire execute() body
// short-circuits to 0 draws + 0 side effects. Headless tests rely
// on this. S1a is the first cut so we also gate on the post-shader
// acquire failure (Phoskia parser may fail without shaderc) — if
// the program never acquired, execute() still returns 0 instead of
// crashing (matches PostProcessPass::execute contract).
//
// K1 invariants (must survive S1b/S1c additions):
//   1. `frame.bloomStrength == 0` ⇒ bright=0 ⇒ half-res FBO writes
//      are zero (visually identical to S1-pre + BloomExtract not
//      in pipeline — zero-behavior-change to existing renders when
//      host keeps default 0 bloomStrength).
//   2. Noop backend ⇒ execute() returns 0 + no FBO created
//      (BGFXAdapter gates FBO create on isInitialized; bloom FBO is
//      lazy just like PostProcessPass FBO).
//   3. half-res size = (viewportW+1)/2 × (viewportH+1)/2 — same
//      rounding as half-res convention; ensure on resize.
//   4. S1a doesn't touch FrameContext / RenderScene / PassExecContext
//      (no field additions). Uses PostProcessPass::selectSourceFbo
//      static helper to read scene color.
//   5. ABI: append-only — RenderPassSlot::BloomExtract = 8 (Lighting
//      was 7); no existing enum value reorders.

#include "AYShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class BloomExtractPass : public RenderPass {
public:
    // Composite view map — must differ from every other slot
    // (FO=0, ShadowC=1, ShadowR=2, Trans=3, PP=4, Skybox=6,
    // GBuffer=7, Lighting=8, Trans-deferred=9, PP-deferred=10,
    // UI=11). View 5 is the only slot left before S1a.
    static constexpr uint8_t kBloomExtractViewId = 5;

    BloomExtractPass() = default;
    // Mirror PostProcessPass: dtor does NOT touch bgfx handles.
    // RenderPass base has no BGFXAdapter reference (passes are
    // adapter-agnostic); BGFXAdapter::shutdown() invalidates all
    // handles globally. For mid-frame adapter teardown, call
    // destroyResources() explicitly first.
    ~BloomExtractPass() override = default;

    std::string_view name() const override { return "BloomExtract"; }

    uint32_t execute(PassExecContext& ctx) override;

    // R5+ mirror — query whether the pass has a real FBO + program
    // wired. Useful for hosts that want to skip the slot via
    // setEnabled(false) when the bloom pipeline cannot be created
    // (e.g. backend was initialized but the Phoskia program is
    // not in the pool). Today "ready" once execute() has built the
    // FBO at least once.
    bool isReady() const noexcept { return bgfx::isValid(_fbo); }

    // Host-facing half-resolution size getter (for tests + future
    // S1b blur that needs to know input size).
    uint16_t halfWidth()  const noexcept { return _fboWidth;  }
    uint16_t halfHeight() const noexcept { return _fboHeight; }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). Mirror PostProcessPass::destroyResources
    // contract. Idempotent (BGFXAdapter::destroy on invalid handle
    // is a no-op).
    void destroyResources(BGFXAdapter& adapter);

private:
    // Lazy FBO — half-resolution RGBA8, no depth. BGFXAdapter owns
    // the bgfx handle; this class owns the cache + size tracking +
    // destroy decision.
    bgfx::FrameBufferHandle    _fbo = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;
    uint16_t                   _fboWidth  = 0;
    uint16_t                   _fboHeight = 0;

    // Phoskia program for the bright-extract effect. Acquired lazily
    // on first execute() after adapter init. Acquire may fail
    // (shaderc missing on CI / disk cache miss + parse error);
    // in that case isReady() stays false and execute() degrades to
    // "bind FBO + return 0" (S1b will read an empty FBO and
    // produce no bloom — visually identical to default bloomStrength=0).
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on first acquire; InvalidBinding
    // means "not yet resolved / acquire failed".
    ayt::shader::BindingId      _uBloomThreshold = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uBloomStrength  = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSceneColor     = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every frame
    // (was the main stutter source in PostProcessPass when
    // Phoskia→HLSL rejected).
    bool                        _programAcquireFailed = false;

    // R5+ helpers — no-ops on the Noop backend (BGFXAdapter gates
    // on isInitialized()), so the headless test path runs clean.
    void ensureFbo(BGFXAdapter& adapter, uint16_t viewportW, uint16_t viewportH);
    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

} // namespace ayt::render::detail