#pragma once

// Half-resolution separable Gaussian blur. It consumes BloomBright only when
// BloomExtract produced it in the current frame, then writes BloomBlurA in
// view 11 and BloomBlurB in view 12. Linear filtering folds the Gaussian
// kernel into five texture fetches per axis.
//
// FrameGraph owns all three RGBA16F targets. BloomBlurA and BloomBlurB cannot
// alias because the vertical pass reads A while writing B. This pass owns only
// fullscreen geometry, temporary attachment handles, and the shader program.
// Production publishes BloomBlurA/B through the resource blackboard after both
// submits complete; `producedThisFrame()` is the direct-context fallback.

#include "AYShader/ShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class BloomBlurPass : public RenderPass {
public:
    // §S1b view map: after BloomExtract=10, before DepthHaze=13.
    // UI is fixed at 255 (not adjacent — leave Post headroom).
    static constexpr uint8_t kBloomBlurHorizontalViewId = 11;
    static constexpr uint8_t kBloomBlurVerticalViewId   = 12;

    BloomBlurPass() = default;
    // Mirror S1a BloomExtractPass + PostProcessPass: dtor does NOT
    // touch bgfx handles. RenderPass base has no BGFXAdapter
    // reference (passes are adapter-agnostic); BGFXAdapter::
    // shutdown() invalidates all handles globally. For mid-frame
    // adapter teardown, call destroyResources() explicitly first.
    ~BloomBlurPass() override = default;

    std::string_view name() const override { return "BloomBlur"; }

    uint32_t execute(PassExecContext& ctx) override;

    // Program readiness is separate from current-frame production and target
    // validity. Production consumers use the resource blackboard; the latch is
    // retained for direct contexts that intentionally omit it.
    bool isReady() const noexcept { return _program.isValid(); }
    bool producedThisFrame() const noexcept { return _producedThisFrame; }
    void resetFrameState() noexcept { _producedThisFrame = false; }

    // Legacy compatibility shims. Physical handles resolve through FrameGraph
    // and are never owned or exposed by this pass.
    bgfx::FrameBufferHandle pingFbo() const noexcept {
        return BGFX_INVALID_HANDLE;
    }
    bgfx::FrameBufferHandle pongFbo() const noexcept {
        return BGFX_INVALID_HANDLE;
    }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). §F3 — F3 ships with no FBO to release;
    // destroyResources only releases the Phoskia program + the
    // fullscreen VB/IB. FG-owned RTs (BloomBlurA / BloomBlurB) are
    // released by FrameGraph::shutdown / FrameGraph::resize (the
    // Renderer's Impl shutdown path calls fg.shutdown()).
    void destroyResources(BGFXAdapter& adapter);

private:
    // §F3 (2026-07-24) — fields removed in F3:
    //   `_pingFbo`          → FrameGraph.BloomBlurA
    //   `_pongFbo`          → FrameGraph.BloomBlurB
    //   `_fboWidth`/`_fboHeight` → FG physical size
    //   `_sourceRt` / `_pingRt` → still kept locally as transient
    //      cache of `adapter.getFboAttachment(handle, 0)` for the
    //      current frame's source / ping attachments (cheap lazy
    //      refresh; mirrors pre-F3 behavior).
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle        _sourceRt     = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    bgfx::TextureHandle        _pingRt       = bgfx::TextureHandle{BGFX_INVALID_HANDLE};

    // §S1b (2026-07-23) — Phoskia program for the separable
    // Gaussian blur effect (single program, branched via uniform
    // `direction` = (1,0) for horizontal, (0,1) for vertical).
    // Acquired lazily on first execute() after adapter init.
    // Acquire may fail (shaderc missing on CI / disk cache miss
    // + parse error); in that case isReady() stays false and
    // execute() degrades to "early-return 0" — visually identical
    // to bloomStrength=0 host (S1a K1 #1 propagated).
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on the first acquire; InvalidBinding
    // means "not yet resolved / acquire failed".
    ayt::shader::BindingId      _uDirection = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uTexelSize = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSource    = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every
    // frame (same stutter source PostProcessPass + S1a
    // BloomExtractPass mitigated).
    bool                        _programAcquireFailed = false;
    bool                        _producedThisFrame = false;

    // R5+ helpers — VB/IB + program acquisition only (FBO ensure
    // removed in F3; FG owns both ping-pong RTs now).
    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

} // namespace ayt::render::detail
