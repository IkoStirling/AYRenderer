#pragma once

// V1 GBuffer Debug (2026-07-24) — GBufferDebugPass skeleton.
//
// SKELETON cut: declares the class shape (view id 250, cache-key
// extern, BindingId field matrix, lifecycle helpers), reserves the
// RenderPassSlot + GBufferDebugChannel enum values, and pins the
// "off == zero alloc + zero draw" contract. Real Phoskia FS per
// channel (Albedo / Normal / WorldPos / Motion / Depth) lands in
// V2. Editor corner widget (256x256 Image + ComboBox channel
// selector + UIRenderBackend textured-rectangle extension) lands
// in V3.
//
// Pipeline position:
//   ... → GBuffer → Lighting → ... → SSAO → PostProcess → UI →
//     GBufferDebug(view 250, runs LAST via bgfx ascending view-id
//     dispatch ⇒ main-frame 0..15 stream byte-identical).
//
// View id allocation (V1 lock):
//   0=FO, 1=ShadowC, 2=ShadowR, 3=Trans, 4=PP-Fwd, 5=BloomExtract,
//   6=Skybox, 7=GBuffer, 8=Lighting, 9=Trans-Def, 10=PP-Def,
//   11=UI, 12=BloomBlurH, 13=BloomBlurV, 14=DepthHaze, 15=SSAO,
//   250=GBufferDebug(V1 new), 255=UI-Editor.
// View 250 verified unused across the repo via `grep -w 250` (0
// hits, 2026-07-24). Sits below 255=UI-editor and above the 0..15
// main-frame stream so the existing passes' submit byte-sequence
// is preserved by construction.
//
// K-GBD invariants (must survive V2 real-shader cut + V3 Editor
// widget cut):
//   1. gbufferDebugEnabled=false OR gbufferPass==nullptr OR
//      uninit/Noop ⇒ render() central gate false ⇒ Renderer::Impl
//      gbufferDebugFbo NOT created (zero alloc) ⇒ ctx.gbufferDebugFbo
//      invalid ⇒ execute() early-returns 0 (zero draw). Double-
//      checked at host gate + execute step-4 (target FBO invalid).
//   2. Phoskia branchless channel select (V2): use `step()` not
//      `if`; depth linearize; normal remap `*0.5+0.5`. Trivially
//      held in V1 (no shader).
//   3. WorldPos(2) / Motion(3) BOTH alias gbufferMotionRt() in V1
//      because GBufferPass has not split a real motion RT from
//      worldPos (RT2 = worldPos RGBA16F per GBufferPass.cpp:49-60).
//      V2 will add a real Motion RT and re-route channel 3 to it.
//   4. ABI: append-only — RenderPassSlot::GBufferDebug = 12 (the
//      value 12 was unused in the post-§S2 slot table). View 250.
//      Default = OFF (host knob; FrameContext::gbufferDebugEnabled
//      = false).
//
// Lifetime model (mirror SSAOPass.h:50-78):
//   - program + BindingId fields are private to this file. Acquire
//     happens lazily on first execute() and on cache-key bump.
//   - debug FBO is HOST-OWNED (Renderer::Impl::gbufferDebugFbo),
//     NOT in FG, NOT in the pass. Mirror sceneFbo host-owned
//     pattern; pass reads via PassExecContext::gbufferDebugFbo
//     borrowed field.
//   - debug pass early-returns 0 when ctx.gbufferDebugFbo is
//     invalid (V1 default behavior + V2/V3 when disabled).

#include "AYShaderResource.h"

#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// V1 GBufferDebugChannel enum (5 logical channels; WorldPos +
// Motion alias gbufferMotionRt() until V2 splits a real motion RT).
enum class GBufferDebugChannel : uint8_t {
    Albedo   = 0,  // RT0 RGBA8 — direct sample, .rgba
    Normal   = 1,  // RT1 RGBA8 — encoded (FS: .xyz*2-1)
    WorldPos = 2,  // RT2 RGBA16F — gbufferMotionRt() direct
    Motion   = 3,  // RT2 RGBA16F — V1 aliases WorldPos; K-GBD-3
    Depth    = 4,  // RT3 D24S8 — linearize in FS
    Count    = 5,
};

class GBufferDebugPass : public RenderPass {
public:
    // V1 view-map lock (cutsheet §G1 V1 lock). 250 = GBufferDebug,
    // verified unused across repo (2026-07-24 grep). Sits below
    // 255=UI-Editor; never perturbs 0..15 main-frame stream.
    static constexpr uint8_t kGBufferDebugViewId = 250;

    // V1 channel count mirror (Test pin + ABI lock).
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

    // V1 stub: program never acquired (V1 ships no real Phoskia FS).
    // isReady() lifts to `_program.isValid()` in V2.
    bool isReady() const noexcept {
        return _program.isValid();
    }

    // Destructor-side release — call BEFORE pipeline.clear() /
    // adapter.shutdown(). V1 ships with no real GPU work yet:
    // destroyResources is a clean no-op (handles stay invalid). V2
    // will fill in VB/IB + program release + binding resets.
    void destroyResources(BGFXAdapter& adapter);

private:
    // ─── V1 skeleton state (mirrors SSAOPass.h:99-127) ──────────
    // V2 fills in the program body + the per-channel Phoskia FS.
    // The BindingId list + signature order is reserved here so
    // tests can pin the field matrix without waiting for V2.
    bgfx::VertexBufferHandle   _fullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle    _fullscreenIB = BGFX_INVALID_HANDLE;

    // Phoskia program — lazy-acquired on first execute() after
    // adapter init (V2). V1: never acquired ⇒ isReady()==false.
    ayt::shader::ShaderResource _program;

    // Cached binding IDs. Resolved on first acquire; InvalidBinding
    // means "not yet resolved / acquire failed". V2 will resolve
    // these after the per-channel Phoskia source lands.
    ayt::shader::BindingId      _uDebugChannel   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tAlbedo         = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tNormal         = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tWorldPos       = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tDepth          = ayt::shader::InvalidBinding;

    // Latch so a failed acquire does not re-run shaderc every frame.
    bool                        _programAcquireFailed = false;

    void ensureFullscreenQuad(BGFXAdapter& adapter);
    void ensureProgram(shader::ShaderResourcePool& pool);
};

// V1 (2026-07-24) — Bug-fix-#3 mirror (same pattern as
// kSSAOCacheKeyCStr in SSAOPass.h:143). Externalize the cache-key
// literal so unit tests can include this header and compare their
// mirror against the live literal. Pre-V2, the literal is a
// placeholder string; V2 bumps it to the real per-channel Phoskia
// shader cache key. The extern declaration gives every test a
// single source of truth; drift = test fails immediately.
extern const char* const kGBufferDebugCacheKeyCStr;

} // namespace ayt::render::detail
