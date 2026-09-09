#pragma once
// AYRenderer/RenderTypes.h — public renderer types (no bgfx / no driver handles)

#include "AYMath/MathTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::render
{

// Capability of the current uniform-buffer skinning backend. It limits one
// draw palette, never the size of an imported/runtime skeleton.
inline constexpr uint32_t kUniformSkinPaletteCapacity = 128u;

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
// with materials created before this PR. Alpha = standard straight-alpha
// "over": RGB uses (srcA, 1-srcA), while coverage alpha uses
// (1, 1-srcA), preserving correct destination alpha for later composition.
enum class BlendMode : uint8_t {
    Opaque = 0,  // default: no blending, depth-write
    Alpha  = 1,  // BGFX_STATE_BLEND_ALPHA (srcA*src + (1-srcA)*dst)
    Additive = 2, // source + destination
};

// Deferred shading model encoded in the GBuffer independently from geometry
// coverage. Values are append-only because assets/tools may persist the byte.
enum class MaterialModel : uint8_t {
    StandardLit = 0,
    Unlit       = 1,
    Count,
};

// A 2D draw can either live in the legacy camera overlay or participate in
// the world/deferred surface pipeline. Values are serialized by host
// components, so keep this enum append-only.
enum class RenderDomain2D : uint8_t {
    SceneOverlay = 0,
    WorldLit     = 1,
    Count,
};

// Material alpha routing for 2D surfaces. The first WorldLit2D slice ships
// Opaque/Cutout through GBuffer; Blend is reserved for the later forward-lit
// transparent stage and therefore currently fails closed in deferred draws.
enum class Material2DAlphaMode : uint8_t {
    Opaque = 0,
    Cutout = 1,
    Blend  = 2,
    Count,
};

// How DrawPayload2D vertex UVs reach the surface samplers. Sprites author a
// source rectangle and use the historical bottom-left atlas convention;
// chunked tilemaps already bake final atlas UVs into their mesh vertices.
enum class UvMapping2D : uint8_t {
    SourceRectBottomLeft = 0,
    BakedAtlas           = 1,
    Count,
};

// Shared between the overlay tilemap shaders and WorldLit2D GBuffer draws.
// Keep values stable because TilemapComponent serializes them as integers.
enum class TilemapSamplingQuality : uint8_t {
    Nearest = 0,
    Linear  = 1,
    Tap4    = 2,
    Tap9    = 3,
    Count,
};

constexpr bool isTransparentBlendMode(BlendMode mode) noexcept
{
    return mode != BlendMode::Opaque;
}

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

struct RenderPassFrameStats {
    std::string name;
    uint32_t    drawCalls = 0;
    float       cpuTimeMs = 0.0f;
    float       gpuTimeMs = 0.0f;
};

struct RenderFrameStats {
    // Wall-clock cadence between consecutive beginFrame calls. Unlike the
    // old render-only timer this includes simulation, event polling, VSync,
    // hot-reload polling and host sleeps, so fps is the rate users see.
    float    fps = 0.0f;
    float    instantaneousFps = 0.0f;
    float    frameTimeMs = 0.0f;
    float    avgFrameTimeMs = 0.0f;
    float    p95FrameTimeMs = 0.0f;
    float    p99FrameTimeMs = 0.0f;

    // CPU time from renderer beginFrame through bgfx::frame(), plus bgfx GPU
    // timestamps. GPU values are zero when the active backend has no timer.
    float    renderCpuTimeMs = 0.0f;
    float    gpuFrameTimeMs = 0.0f;

    // drawCalls is the sum reported by RenderPass implementations;
    // backendDrawCalls is bgfx's authoritative submitted draw count.
    uint32_t drawCalls = 0;
    uint32_t backendDrawCalls = 0;
    uint32_t backendBlitCalls = 0;
    uint32_t sceneItems = 0;
    uint64_t frameCount = 0;
    uint32_t gpuFrameNumber = 0;

    std::vector<RenderPassFrameStats> passes;
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
    BoneIndices,   // uint4 (4x u8, non-normalized) — draw-local palette slots
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

    // Skinning addon: BoneIndices (4x u8 non-normalized) + BoneWeights (4x f32).
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

// Public, GPU-handle-only description of a shared 2D surface material.
// Per-instance UV/tint/flip data remains in DrawPayload2D; maps and scalar
// surface properties live here so they can be cached and shared.
struct Material2DDesc {
    TextureHandle albedo{};       // required
    TextureHandle normal{};       // optional; flat +Z fallback
    TextureHandle roughnessMap{}; // optional; white fallback
    TextureHandle emissiveMap{};  // optional; white fallback

    float metallic        = 0.0f;
    float roughness       = 0.75f;
    float ambientOcclusion = 1.0f;
    float emissiveStrength = 0.0f;
    float alphaCutoff      = 0.5f;
    Material2DAlphaMode alphaMode = Material2DAlphaMode::Cutout;
    bool invertNormalY = false;
    bool doubleSided   = true;
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
    // append-only ABI value 11. Full-resolution GBuffer SSAO pass inserted
    // AFTER GBuffer + BEFORE Lighting on
    // the **Deferred-only** pipeline (cutsheet §S2 hard line:
    // makeDefault() does NOT mount this slot — host must call
    // makeDeferred() or a custom desc that includes it). Forward
    // pipeline never sees this slot. Stable view id 14 is ordered explicitly
    // before Lighting view 8; Haze remains view 13 and PostProcess view 15.
    //
    // K-SSAO invariants:
    //   1. When frame.ssaoEnabled=false (host default) OR
    //      ssaoStrength<=0 OR gbufferPass==nullptr, the render()
    //      central `ssaoPassEnabled` is false ⇒ FG compile culls
    //      SSAOTexture ⇒ SSAOPass::execute early-returns 0. Lighting is the
    //      only consumer and applies AO to ambient/IBL, never direct,
    //      emissive, unlit, haze, or final post-process color.
    //   2. ABI: append-only. NEVER reorder or repurpose existing
    //      slot values. Test pin: static_cast<uint8_t>(
    //      RenderPassSlot::SSAO) == 11.
    SSAO = 11,

    // Deferred-only fullscreen GBuffer attachment overlay on view 250.
    // Default OFF; append-only ABI value 12 must not be reordered.
    GBufferDebug = 12,

    // Editor overlay (2026-07-25) — append-only ABI value 13. It owns the
    // orientation widget on view 251 after Present. TransparentPass generates
    // the depth-aware selection mask earlier on view 253, then composites its
    // screen-space dilation to the backbuffer on view 254 after Present. This
    // keeps the fixed-width rim out of TAA/Bloom while preserving occlusion.
    // Omitted from makeDefault() / makeDeferred(); editor hosts opt in via
    // makeEditorForward() / makeEditorDeferred().
    EditorOverlay = 13,

    // 2D opaque lane (2026-08-11, CM-1) — append-only ABI value 14.
    // Draws every DrawItem carrying a `DrawPayload2D*` (see
    // AYRenderer/RenderScene.h) with alpha blending and no depth test/write.
    // The pass uses RenderScene's independent 2D overlay camera, executes
    // after 3D transparency in both canonical pipelines, and performs one
    // global stable sort by packedSortKey. 3D passes reject payload items.
    Forward2DOpaque = 14,

    // Mandatory final presentation stage — append-only ABI value 15.
    // PostProcess writes FrameGraph FinalLdrColor on view 15; Present samples
    // it and writes the editor/game viewport rect on the default backbuffer
    // using view 16. Cheap post-tonemap effects can now be inserted between
    // these slots without reopening FinalPP's backbuffer contract.
    Present = 15,

    // Display-referred fast approximate anti-aliasing between PostProcess and
    // Present. Append-only ABI value 16; stable bgfx view id 17 is explicitly
    // ordered before Present view 16.
    FXAA = 16,

    // Display-referred 2D LUT color grading after FXAA and before Present.
    // Append-only ABI value 17; renderer defaults disabled so existing hosts
    // do not allocate its target or procedural LUT until explicitly enabled.
    ColorGrading = 17,

    // Three-stage display-referred SMAA 1x. Append-only ABI value 18; mounted
    // after FXAA and before ColorGrading, but runtime AA knobs keep FXAA and
    // SMAA mutually exclusive.
    SMAA = 18,

    // Deferred temporal anti-aliasing. Append-only ABI value 19. It executes
    // after PostProcess and before the display-referred spatial AA slots;
    // runtime AA knobs keep TAA/FXAA/SMAA mutually exclusive.
    TAA = 19,

    // Deferred-only RG16F screen-space velocity producer. Append-only ABI
    // value 20. It replays opaque geometry after GBuffer while borrowing the
    // GBuffer depth attachment, and is requested only by temporal consumers
    // such as TAA.
    MotionVector = 20,
};

enum class ColorGradingPreset : uint8_t {
    Neutral   = 0,
    Warm      = 1,
    Cool      = 2,
    Cinematic = 3,
};

// Editor-only transform manipulator. Handle ids are intentionally opaque to
// the renderer: AYEditor owns hit testing and drag semantics, while the
// EditorOverlay pass uses activeHandle only to highlight matching geometry.
enum class EditorTransformGizmoMode : uint8_t {
    Hidden = 0,
    Translate,
    Rotate,
    Scale,
    Universal,
};

struct EditorTransformGizmoState {
    bool visible = false;
    bool localSpace = false;
    EditorTransformGizmoMode mode = EditorTransformGizmoMode::Hidden;
    uint8_t activeHandle = 0;
    // Bit N disables handle id N. Disabled handles remain as muted visual
    // orientation cues but AYEditor excludes them from CPU hit testing.
    uint16_t disabledHandleMask = 0;
    ayt::math::FVector3 position{};
    ayt::math::FQuaternion rotation = ayt::math::FQuaternion::identity();
};

// AYEditor compile-time checks this value against its CPU hit geometry so the
// gizmo keeps an approximately constant screen size as the camera dollies.
inline constexpr float kEditorTransformGizmoScalePerDistance = 0.18f;

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
    // but insert RenderPassSlot::EditorOverlay immediately after Present
    // (selection rim on backbuffer; stable color, no bloom/tonemap).
    static RenderPipelineDesc makeEditorForward();
    static RenderPipelineDesc makeEditorDeferred();

    bool contains(RenderPassSlot slot) const noexcept;
    bool isDeferred() const noexcept { return path == RenderPath::Deferred; }
};

// ─────────────────────────────────────────────────────────────────────
// §P2 M9+M10 (2026-08-24) — hoist magic numbers used by Pass files
// into the public header. Both constants were duplicated across
// ForwardOpaquePass (clear color, two sites) and TransparentPass
// (view id 9, single site). Pulling them here keeps the cut-sheet
// red-line "one source of truth" rule and gives test code a place
// to verify the values.
// ─────────────────────────────────────────────────────────────────────

// §P2 M9 (2026-08-24) — editor / scene clear color used by
// ForwardOpaquePass (both FBO-bound and backbuffer-bound branches),
// RendererSubSystem composite clear, and any future Pass that wants
// to clear an offscreen RT to match the host's "panel placeholder"
// color. Stored as 0xAABBGGRR (bgfx packed layout).
inline constexpr uint32_t kSceneClearRgba = 0x191a1cffu;

// §P2 M10 (2026-08-24) — view id dedicated to TransparentPass when
// the active composite target is LightingPass::lightingOutputFbo()
// (deferred-lit composite path). Distinct from the Forward path's
// view id (0) because bgfx binds one FBO+VP per view for the whole
// frame; switching views mid-frame is the only way to retarget the
// transparent composite without disturbing the GBuffer / Lighting
// views (7/8). The cut-sheet comment-table elsewhere in this file
// enumerates the slot assignments; this constant is the runtime
// reflection of that table.
inline constexpr uint8_t kTransparentDeferredViewId = 9;

// §P3 M4 (2026-08-24) — hoisted magic clear colors used by the
// deferred path. The editor / composite stack expects a black
// backdrop for the lighting output (0x000000ff = transparent black;
// the alpha byte is preserved by bgfx's packed 0xAABBGGRR layout)
// and a fully-zero GBuffer RT3 (the explicit geometry-coverage
// channel starts at 0 and every successful GBuffer draw writes 1).
// Centralizing these values here prevents bit-drift if a future
// "match editor background" cut picks a non-black color.
inline constexpr uint32_t kLightingClearRgba = 0x000000ffu;
inline constexpr uint32_t kGBufferClearRgba  = 0x00000000u;

// §P3 L12 (2026-08-24) — IBL ambient intensity default. Mirrored
// from LightingPass::_ambientStrength's hard-coded initializer so a
// future Renderer::setAmbientStrength(float) can use the same
// value without a magic-number round-trip.
inline constexpr float kDefaultAmbientStrength = 0.6f;

// §P3 M1 (2026-08-24) — skyMix uniform default. The LightingPass
// and SkyboxPass both upload this every frame; no per-material
// override hook exists today (the audit caught a misleading comment
// that claimed one did). Documenting the constant here is the
// interim fix until a future cut ships the per-material lookup.
inline constexpr float kDefaultSkyMix = 1.0f;

} // namespace ayt::render
