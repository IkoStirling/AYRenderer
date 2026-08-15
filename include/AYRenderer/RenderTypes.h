#pragma once
// AYRenderer/RenderTypes.h — public renderer types (no bgfx / no driver handles)

#include "AYMath/MathTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::render
{

enum class Backend : uint8_t {
    Auto = 0,
    Direct3D11,
    Direct3D12,
    Vulkan,
    OpenGL,
    Metal,
    Noop,  // headless / unit tests only
};

// U1 — material-level blend state. Default = Opaque for source-compat
// with materials created before this PR. Alpha = standard "over"
// compositing via BGFX_STATE_BLEND_ALPHA — assumes the shader's
// fragment output is non-premultiplied vec4(rgb, alpha), which is the
// Phoskia default output shape.
enum class BlendMode : uint8_t {
    Opaque = 0,  // default: no blending, depth-write
    Alpha  = 1,  // BGFX_STATE_BLEND_ALPHA (srcA*src + (1-srcA)*dst)
};

struct InitDesc {
    void*    windowHandle = nullptr;
    uint32_t width        = 1280;
    uint32_t height       = 720;
    bool     vsync        = true;
    Backend  backend      = Backend::Auto;
    bool     enableDebugOverlay = false;
    // Backbuffer MSAA sample count: 0 (off), 2, 4, 8, or 16.
    // Style tip: set 0 for crisp/stylized silhouettes.
    uint32_t msaa = 4;
    // Soft shadow filter (3x3 PCF). Style tip: false → hard 1-tap edges.
    bool     shadowPcf = true;
};

struct RenderFrameStats {
    float    fps = 0.0f;
    float    frameTimeMs = 0.0f;
    float    avgFrameTimeMs = 0.0f;
    uint32_t drawCalls = 0;
    uint32_t sceneItems = 0;
    uint64_t frameCount = 0;
};

struct ClearDesc {
    float r = 0.05f;
    float g = 0.05f;
    float b = 0.08f;
    float a = 1.0f;
    bool  clearDepth = true;
};

// Vertex layout description (OpenGL-style attributes, no bgfx types in public API).
enum class VertexAttribute : uint8_t {
    Position,
    Normal,
    TexCoord0,
    Tangent,
    Color0,
    BoneIndices,   // uint4 (4x u8, normalized) — for skeletal skinning (Phase 0 RD-02)
    BoneWeights,   // float4 (4x f32)            — for skeletal skinning (Phase 0 RD-02)
};

enum class VertexComponentType : uint8_t {
    Float,
    Uint8,
};

struct VertexElement {
    VertexAttribute     attribute = VertexAttribute::Position;
    uint8_t             componentCount = 3;
    VertexComponentType componentType = VertexComponentType::Float;
    bool                normalized = false;
};

struct VertexLayoutDesc {
    static constexpr uint8_t kMaxElements = 10;

    VertexElement elements[kMaxElements]{};
    uint8_t       elementCount = 0;

    bool add(const VertexElement& element);
    bool isValid() const noexcept;
    uint32_t strideBytes() const noexcept;

    // Common presets (logical attribute order; bgfx may add padding — repack on upload).
    static VertexLayoutDesc position3();
    static VertexLayoutDesc position3Normal3();
    static VertexLayoutDesc position3TexCoord2();
    static VertexLayoutDesc position3Normal3TexCoord2();

    // Skinning addon: BoneIndices (4x u8 normalized) + BoneWeights (4x f32).
    // 24 bytes total. Phase 0 RD-02.
    static VertexLayoutDesc skinnedAddon();
};

struct MeshHandle {
    uint64_t id = 0;
    bool isValid() const noexcept { return id != 0; }
};

struct MaterialHandle {
    uint64_t id = 0;
    bool isValid() const noexcept { return id != 0; }
};

struct TextureHandle {
    uint64_t id = 0;
    bool isValid() const noexcept { return id != 0; }
};

// Phase 4 — per-draw shadow participation (caster pass + receiver compare).
enum class ShadowFlags : uint8_t {
    None    = 0,
    Cast    = 1u << 0,
    Receive = 1u << 1,
};

inline constexpr ShadowFlags operator|(ShadowFlags a, ShadowFlags b) noexcept
{
    return static_cast<ShadowFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline constexpr ShadowFlags operator&(ShadowFlags a, ShadowFlags b) noexcept
{
    return static_cast<ShadowFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline constexpr ShadowFlags kShadowCastAndReceive =
    ShadowFlags::Cast | ShadowFlags::Receive;

inline bool castsShadow(ShadowFlags flags) noexcept
{
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(ShadowFlags::Cast)) != 0;
}

inline bool receivesShadow(ShadowFlags flags) noexcept
{
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(ShadowFlags::Receive)) != 0;
}

inline ShadowFlags makeShadowFlags(bool cast, bool receive) noexcept
{
    ShadowFlags flags = ShadowFlags::None;
    if (cast) {
        flags = flags | ShadowFlags::Cast;
    }
    if (receive) {
        flags = flags | ShadowFlags::Receive;
    }
    return flags;
}

// Host-facing pass slots for RenderPipelineDesc. Order in
// `RenderPipelineDesc::passes` is the dispatch order used by
// Renderer::configurePipeline. Default product pipeline includes
// Shadow enabled (E5 §5.4, 2026-07-22); Editor / demos that want
// shadow disabled pass a custom desc that omits the Shadow slot.
// makeForwardWithShadows() is now an alias for makeDefault().
enum class RenderPassSlot : uint8_t {
    Shadow = 0,
    // §Skybox0 (2026-07-23) — slot 1, between Shadow and GBuffer.
    // The SkyboxPass writes an independent RGBA8 FBO (skyFbo) that
    // LightingPass samples as a backdrop (`texture2d gbufferSky`).
    // Default Forward pipeline (RenderPipelineDesc::makeDefault())
    // does NOT include this slot — Skybox is opt-in via
    // `configurePipeline(makeDeferred())` or a custom desc. Forward
    // hosts see 0 behavior change (cutsheet §5.3 red line #4).
    Skybox,
    ForwardOpaque,
    Transparent,
    PostProcess,
    UI,
    // §P5 B3 (2026-07-22) — Deferred-only slots. GBuffer = view 7
    // (B4 MRT fill), Lighting = view 8 (B5 fullscreen triangle).
    // Both OMITTED from makeDefault() / Forward path (E5 "omit slot
    // = opt out" semantics). Reserved values for ABI stability;
    // never reorder without a cutsheet-style B0 reset.
    GBuffer,
    Lighting,
    // S1a BloomExtract (2026-07-23, short-term-plan §S1) — half-
    // resolution bright-extract pass inserted between Transparent
    // and PostProcess on BOTH Forward + Deferred default pipelines.
    // View 5 was the only unused slot before S1a (composite view
    // table: 0=FO, 1=ShadowC, 2=ShadowR, 3=Trans, 4=PP, 6=Skybox,
    // 7=GBuffer, 8=Lighting, 9=Trans-deferred, 10=PP-deferred,
    // 11=UI). When bloomStrength == 0 (the host default), the
    // half-res FBO writes are zero ⇒ pre-S1 zero-behavior-change
    // (cutsheet §S1 K1 invariant #1). Omit this slot in a custom
    // desc to disable the entire bloom chain (forward-compat for
    // S1b/S1c).
    BloomExtract,
    // S1b BloomBlur (2026-07-23, short-term-plan §S1 sub-cut 2) —
    // half-resolution separable-Gaussian blur ping-pong pass
    // inserted AFTER BloomExtract on BOTH Forward + Deferred
    // default pipelines. Views 12 (horizontal) + 13 (vertical)
    // were the only contiguous pair of unused view ids before
    // S1b (composite view table now: 0=FO, 1=ShadowC, 2=ShadowR,
    // 3=Trans, 4=PP, 5=BloomExtract, 6=Skybox, 7=GBuffer,
    // 8=Lighting, 9=Trans-deferred, 10=PP-deferred, 11=UI,
    // 12=BloomBlurH, 13=BloomBlurV). When bloomStrength == 0
    // (host default) the blur samples are zero ⇒ pre-S1
    // zero-behavior-change (cutsheet §S1 K1 invariant #1
    // propagated). Omit this slot to disable the blur while
    // keeping extract (forward-compat for hosts that want only
    // the bright-extract stage, e.g. for custom compositing).
    BloomBlur,

    // §S4a (2026-07-23, short-term-plan §S4 sub-cut 1) — append-only
    // ABI value: DepthHaze takes slot 10 (the value 10 was unused
    // in the §S1 composite view table). Half-resolution depth-aware
    // haze pass inserted AFTER BloomBlur + BEFORE PostProcess on
    // BOTH Forward + Deferred default pipelines (per §S4 决策
    // 2026-07-23: "haze 只改 raw, bloom 独立" ⇒ haze reads
    // *un-bloomed* sceneColor/lightingOutput, bloom chain is
    // untouched). View 14 (first contiguous unused id after S1b's
    // 12/13). Reserved for §S4b; §S4a only adds the slot.
    //
    // When `hazeStrength == 0` (host default) the haze sampler in
    // PostProcessPass (§S4c) collapses to sceneColor ⇒
    // pre-S4 zero-behavior-change (K3 invariant mirrored from §S1c
    // for the haze sampler). Omit this slot in a custom desc to
    // disable haze entirely (forward-compat for hosts that want
    // neither bloom nor haze).
    //
    // ABI: append-only. NEVER reorder or repurpose existing slot
    // values (mirror §S1 cutsheet append-only rule, cutsheet
    // `docs/pass-lessons-from-deferred.md` §7).
    DepthHaze = 10,

    // §A1 SSAO MVP (2026-07-24, mid-term FG MVP SSAO Gate) —
    // append-only ABI value 11. Full-resolution worldPos-sphere
    // SSAO pass inserted AFTER DepthHaze + BEFORE PostProcess on
    // the **Deferred-only** pipeline (cutsheet §S2 hard line:
    // makeDefault() does NOT mount this slot — host must call
    // makeDeferred() or a custom desc that includes it). Forward
    // pipeline never sees this slot. View 14 claim (cutsheet §S2
    // view-map lock: Haze=13 → SSAO=14 → PP=15).
    //
    // K-SSAO invariants (must survive A2 pipeline wire + A3 composite):
    //   1. When frame.ssaoEnabled=false (host default) OR
    //      ssaoStrength<=0 OR gbufferPass==nullptr, the render()
    //      central `ssaoPassEnabled` is false ⇒ FG compile culls
    //      SSAOTexture ⇒ SSAOPass::execute early-returns 0 ⇒
    //      PostProcessPass composite gate folds `step(0.0001,
    //      ssaoStrength.x)` to 0 — byte-equivalent composite.
    //   2. ABI: append-only. NEVER reorder or repurpose existing
    //      slot values. Test pin: static_cast<uint8_t>(
    //      RenderPassSlot::SSAO) == 11.
    SSAO = 11,

    // V1 GBuffer Debug (2026-07-24) — append-only ABI value 12
    // (SSAO=11 was the previous max). Independent second viewport
    // on view 250; does NOT touch main render order; PostProcess
    // composite byte-equivalent. Default-OFF (host knob).
    // makeDefault() does NOT include this slot — Forward path
    // never sees it. makeDeferred() appends it as the LAST entry
    // (after UI) so the bgfx ascending view-id dispatch places
    // view 250 strictly after view 15, keeping the 0..15
    // main-frame stream byte-identical.
    //
    // K-GBD invariants (must survive V2 real-shader cut + V3
    // Editor widget cut):
    //   1. gbufferDebugEnabled=false OR gbufferPass==nullptr OR
    //      uninit/Noop ⇒ render() central gate false ⇒ FBO not
    //      created (zero alloc) ⇒ ctx FBO invalid ⇒ execute()
    //      returns 0 (zero draw). Double-checked at host gate +
    //      execute step-4.
    //   2. ABI: append-only. NEVER reorder or repurpose existing
    //      slot values. Test pin: static_cast<uint8_t>(
    //      RenderPassSlot::GBufferDebug) == 12.
    GBufferDebug = 12,

    // Editor selection overlay (2026-07-25) — append-only ABI value 13.
    // Inverted-hull selection rim drawn AFTER PostProcess (stable color,
    // no bloom/tonemap) and BEFORE UI. Omitted from makeDefault() /
    // makeDeferred(); editor hosts opt in via makeEditorForward() /
    // makeEditorDeferred(). View 16 (PostProcess=15, UI=255).
    EditorOverlay = 13,

    // 2D opaque lane (2026-08-11, CM-1) — append-only ABI value 14.
    // Draws every DrawItem carrying a `DrawPayload2D*` (see
    // AYRenderer/RenderScene.h) with alpha blending, NO depth test/write —
    // ortho z=0 self-occlusion would let depth discard later draws,
    // so the CPU-side packedSortKey order IS the final order
    // (AY2D design.md §7.4 stable_sort semantics). Material must
    // stay BlendMode::Opaque: an Alpha material here would also be
    // submitted by TransparentPass (double draw). Forward path only
    // — makeDeferred() does NOT include this slot (cutsheet §S2
    // hard-line style: "omit slot = opt out").
    Forward2DOpaque = 14,
};

// §P5 B1 (2026-07-22) — pipeline path selection. B1 ship was
// plumbing only: `RenderPipelineDesc::path` field (default Forward)
// + `makeDeferred()` stub returning the same 5-slot Forward list.
// §P5 B3 (2026-07-22) — `makeDeferred()` is now actual: 6-slot
// pipeline {Shadow, GBuffer, Lighting, Trans, PP, UI} with
// ForwardOpaque OMITTED per cutsheet §4.1 red line #4. The path
// enum itself stays unchanged; only the slot list returned by
// `makeDeferred()` differs from `makeDefault()`.
//
// Forward path remains opt-out (host passes a custom desc that
// omits GBuffer/Lighting, or sticks with `makeDefault()`). Deferred
// path remains opt-in via `configurePipeline(makeDeferred())`;
// default Forward behavior is unchanged.
enum class RenderPath : uint8_t {
    Forward  = 0,
    Deferred = 1,
};

struct RenderPipelineDesc {
    std::vector<RenderPassSlot> passes;
    // §P5 B1 (2026-07-22) — pipeline path. Default Forward keeps the
    // current 5-slot behavior intact; Deferred becomes meaningful
    // from B3 onwards (GBuffer + Lighting slots, view 7/8).
    RenderPath path = RenderPath::Forward;

    static RenderPipelineDesc makeDefault();
    static RenderPipelineDesc makeForwardWithShadows();
    // §P5 B1 stub — same 5-slot Forward pipeline but tagged
    // `path=Deferred`. Actual Deferred behavior lands in B3; until
    // then, a host that calls `configurePipeline(makeDeferred())`
    // sees no behavioral difference (intentional — the B1 commit
    // pre-wires the plumbing without touching dispatch order or
    // GPU resources).
    static RenderPipelineDesc makeDeferred();
    // Editor pipelines — same as makeForwardWithShadows() / makeDeferred()
    // but insert RenderPassSlot::EditorOverlay immediately after PostProcess
    // (selection rim on backbuffer; stable color, no bloom/tonemap).
    static RenderPipelineDesc makeEditorForward();
    static RenderPipelineDesc makeEditorDeferred();

    bool contains(RenderPassSlot slot) const noexcept;
    bool isDeferred() const noexcept { return path == RenderPath::Deferred; }
};

} // namespace ayt::render
