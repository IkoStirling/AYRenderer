#pragma once

// Half-resolution HDR bright extraction. The pass reads the current scene
// color selected by PostProcessPass, performs a five-sample Karis-weighted
// downsample, then applies a scene-linear threshold with a configurable soft
// knee. FrameGraph owns the RGBA16F BloomBright target; this pass owns only
// its fullscreen geometry and shader program.
//
// The renderer treats Extract + Blur as one capability: an incomplete or
// disabled chain is not declared, and strength <= 0 allocates no bloom RTs.
// `producedThisFrame()` is reset before every dispatch and becomes true only
// after a successful submit, so downstream passes cannot consume stale data.
//
// View order is fixed: Transparent=9, Extract=10, BlurH=11, BlurV=12,
// DepthHaze=13, SSAO=14, PostProcess=15, UI=255.

#include "AYShader/ShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

class BloomExtractPass : public RenderPass {
public:
    // Composite view map (S1 bloom order lock):
    //   … Trans-deferred=9 → BloomExtract=10 → BlurH=11 → BlurV=12
    //   → PostProcess=13 → UI=255 (fixed high slot).
    static constexpr uint8_t kBloomExtractViewId = 10;

    BloomExtractPass() = default;
    // Mirror PostProcessPass: dtor does NOT touch bgfx handles.
    // RenderPass base has no BGFXAdapter reference (passes are
    // adapter-agnostic); BGFXAdapter::shutdown() invalidates all
    // handles globally. For mid-frame adapter teardown, call
    // destroyResources() explicitly first.
    ~BloomExtractPass() override = default;

    std::string_view name() const override { return "BloomExtract"; }

    uint32_t execute(PassExecContext& ctx) override;

    // Program readiness is separate from per-frame production and FG target
    // validity; consumers must use producedThisFrame().
    bool isReady() const noexcept { return _program.isValid(); }
    bool producedThisFrame() const noexcept { return _producedThisFrame; }
    void resetFrameState() noexcept { _producedThisFrame = false; }

    // Legacy compatibility shims. Physical dimensions and handles are owned
    // by FrameGraph and require a frame context to resolve.
    uint16_t halfWidth()  const noexcept { return 0; }
    uint16_t halfHeight() const noexcept { return 0; }

    bgfx::FrameBufferHandle halfResFbo() const noexcept {
        return BGFX_INVALID_HANDLE;
    }

    // §F2 (2026-07-24) — destroyResources 保留,但只释放 program +
    // VB + IB。FG own 的 transient RT 由 FrameGraph::shutdown 释
    // 放(在 Impl shutdown 路径调 fg.shutdown())。调用者 (Render
    // Pipeline teardown) 仍先调 destroyResources,确保 program
    // handle 计数归零后再让 ShaderResourcePool dtor 释放底层
    // GPU program。
    void destroyResources(BGFXAdapter& adapter);

private:
    // Transient RT ownership lives entirely in FrameGraph.
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;

    // Acquired lazily. Failure leaves the current-frame production latch
    // false, which makes Blur and Final compositing fail closed.
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on first acquire; InvalidBinding
    // means "not yet resolved / acquire failed".
    ayt::shader::BindingId      _uBloomThreshold = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uSourceTexelSize = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSceneColor     = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every frame
    // (was the main stutter source in PostProcessPass when
    // Phoskia→HLSL rejected).
    bool                        _programAcquireFailed = false;
    bool                        _producedThisFrame = false;

    // R5+ helpers — no-ops on the Noop backend (BGFXAdapter gates
    // on isInitialized()), so the headless test path runs clean.
    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

} // namespace ayt::render::detail
