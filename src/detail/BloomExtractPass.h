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
// View id allocation: Trans-deferred=9 → BloomExtract=10 → Blur →
// PostProcess=13 → UI=255 (fixed high slot).
// View id matches the cutsheet view-id table (lock per
// docs/execution-plan.md §5.1 — append-only / order-critical).
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
//   1. `frame.bloomStrength == 0` ⇒ Final PP adds zero bloom (extract
//      still writes the bright plate; strength is Final-only).
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

    // R5+ mirror — query whether the pass has a real FBO + program
    // wired. Useful for hosts that want to skip the slot via
    // setEnabled(false) when the bloom pipeline cannot be created
    // (e.g. backend was initialized but the Phoskia program is
    // not in the pool). Today "ready" once execute() has built the
    // FBO at least once.
    // §F2 (2026-07-24) — F2 ships with FG 物理创建延后到 F6;isReady
    // 反映"FG 是否真创建了 BloomBright" ── 当前恒 false,直到 F6
    // 真打开物理 RT 创建。Pass::execute() 不依赖 isReady() 决定
    // 0/non-0 draw ── 它直接看 ctx.frameGraph->resolve(BloomBright)
    // 的返值。
    bool isReady() const noexcept { return false; }

    // §F2 (2026-07-24) — halfWidth/halfHeight 退化为"FG 物理尺寸
    // getter"。F2 阶段 FG 物理创建未开 ⇒ 返 0。F6 真打开后,这些
    // getter 改读 ctx.frameGraph 的 live physicalW/H(F6 自己定
    // 细节,不是 F2)。
    uint16_t halfWidth()  const noexcept { return 0; }
    uint16_t halfHeight() const noexcept { return 0; }

    // §S1b (2026-07-23, §F2 updated 2026-07-24) — consumer entry
    // point. The downstream BloomBlurPass reads this FBO (its RT0
    // attachment is sampled as the blur source) and ping-pongs into
    // two of its own halfW × halfH FBOs. Mirrors LightingPass::
    // lightingOutputFbo() producer-state pattern.
    //
    // §F2 update:halfResFbo() 之前返 `_fbo`(Pass own);现在需 ctx
    // 才能问 FG ── 但 consumer (BloomBlurPass) 只在 F3 才迁入
    // FG,这一帧(2026-07-24 F2)BloomBlurPass 仍按旧约定调
    // halfResFbo(),所以保持一个 stateless getter 返 invalid 占位。
    // F3 会改 BloomBlurPass 走 ctx.frameGraph->resolvePingPong,
    // 并删除本 getter。
    //
    // 返回 BGFX_INVALID_HANDLE 当 FG 物理创建未开(F2 阶段 ── 字节
    // 与 host bloomStrength=0 一致);F6 真打开后此 getter 被移除
    // (consumer 改问 FG)。
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
    // §F2 (2026-07-24) — `_fbo/_fboWidth/_fboHeight` 已迁出到
    // FrameGraph。BloomExtract 不再 own 自己的 transient RT。Pass
    // 只 own:Phoskia program / fullscreen VB / IB。物理 RT 走
    // ctx.frameGraph->resolve(FgResourceId::BloomBright)。
    //
    // 仍有私有字段:VB / IB(program 是 ShaderResource 包装类)。
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;

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