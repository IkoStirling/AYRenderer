#include "detail/PostProcessPass.h"

#include "detail/BloomBlurPass.h"  // §S1c (2026-07-23) — PongFbo access
#include "detail/DepthHazePass.h"  // §S4c (2026-07-23) — halfResFbo() access
#include "detail/FgResource.h"    // §F5 (2026-07-24) — BloomSource / HazeSource semantic
#include "detail/GpuResources.h"
#include "detail/GBufferPass.h"
#include "detail/LightingPass.h"

#include "AYShaderResource.h"

#include <cstdio>
#include <cstring>

namespace ayt::render::detail
{

namespace {

// R5+ — fullscreen-triangle vertex data. 3 verts in NDC: a single
// oversize triangle covers the entire screen, no index buffer needed
// past 3 indices. Using a triangle (vs. a 4-vert quad) avoids the
// diagonal seam across adjacent pixels — bgfx's fullscreen-quad
// examples use the same pattern (see bgfx/examples/00-helloworld).
struct alignas(16) FullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr FullscreenVertex kFullscreenTriangle[3] = {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    {  3.0f, -1.0f, 2.0f, 1.0f },
    { -1.0f,  3.0f, 0.0f, -1.0f },
};

constexpr uint16_t kFullscreenIndices[3] = { 0, 1, 2 };

// Post-process: sample sceneColor → exposure → optional bloom tint →
// branchless tonemap (None / Reinhard / ACES) → display gamma.
//
// §S1c (2026-07-23, short-term-plan §S1 sub-cut 3) — added a second
// sampler `bloomTexture` that, when bound by execute() to
// `ctx.bloomBlurPass->pongFbo()` RT0, replaces the pre-S1 fake
// `raw + raw*bloomStrength` shader hack with the real composite
// `raw + sample(bloomTexture, uv) * bloomStrength`. When the
// bloomTexture sampler is NOT bound (custom desc omits BloomExtract
// + BloomBlur, or first-frame race), the FS branchless mix collapses
// to `raw * (1 + 0) = raw` — visually identical to a zero-bloom
// pipeline (cutsheet §S1 §K3 invariant #1). Branchless (converter
// drops if/for). Knobs are vec4 (.x) for bgfx Vec4 upload ABI — see
// docs/pass-lessons-from-shadow.md §3.1. tonemapMode.x: 0=None,
// 1=Reinhard, 2=ACES (Narkowicz fitted). Select via
// mix(mix(none, reinhard, step(0.5,m)), aces, step(1.5,m)).
//
// UV.y flip in fragment (1 - vUv.y): Phoskia vertex blocks reject
// `let` before `out`. Same D3D RT vs backbuffer convention as shadows.
constexpr const char* kPostProcessPhoskiaSource = R"(
material PostProcess {
    texture2d sceneColor
    texture2d bloomTexture
    texture2d hazeTexture
    texture2d ssaoTexture
    uniform vec4 bloomStrength
    uniform vec4 exposure
    uniform vec4 tonemapMode
    uniform vec4 uTime
    uniform vec4 gammaParams
    uniform vec4 hazeDensity
    uniform vec4 hazeStrength
    uniform vec4 hazeColor
    uniform vec4 ssaoStrength
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let sampled = sample(sceneColor, uv)
        let bloomSample = sample(bloomTexture, uv)
        // §S4c — sample the haze result RT0 (when bound by execute()
        // to ctx.depthHazePass->halfResFbo(); otherwise the sampler
        // returns the unbound sentinel = the same RT0 as sceneColor
        // since both share the BGRA8 layout). The composite uses
        // `hazeSample.w` (the haze FBO's alpha channel) as the dist
        // proxy — the DepthHazePass S4b FS writes the un-hazed raw
        // alpha there (`return vec4(mixed, raw.w)`), so
        // `exp(-density * 0) = 1` and `1 - 1 = 0` ⇒ fogFactor
        // collapses to 0 when the strength gate is off (K3 invariant
        // #3 branchless).
        let hazeSample = sample(hazeTexture, uv)
        let raw = sampled.xyz * exposure.x
        // §S4c fix (2026-07-23) — DepthHazePass already wrote the
        // exponential fog mix into hazeSample.rgb. Final PP must NOT
        // re-derive fog from alpha (raw.w was never a distance proxy).
        // Prefer the pre-hazed color when hazeStrength > 0; when the
        // haze slot falls back to sceneColor the mix is a no-op.
        let hazeWeight = step(0.0001, hazeStrength.x)
        let rawHaze = mix(raw, hazeSample.xyz * exposure.x, hazeWeight)
        // §A3 SSAO MVP (2026-07-24, mid-term FG MVP SSAO Gate) —
        // occlusion factor sample. SSAOPass writes R = occlusion
        // ∈ [0,1] onto SSAOTexture. Composite applies multiplicative
        // darkening `rawHaze * (1 - occlusion)`. When the SSAO slot
        // falls back to sceneColor (K-SSAO-1: ssaoEnabled=false ⇒
        // FG compile culls SSAOTexture ⇒ resolve returns invalid ⇒
        // sampler bound to sceneColor), the .r sample is NOT 1.0
        // (it's `raw.r * exposure.x`), so the `step(0.0001,
        // ssaoStrength.x)` strength gate MUST be the folding
        // mechanism, not the sampler. Phoskia has no `saturate`
        // builtin ⇒ collapse via `clamp(1 - x, 0, 1)` per K-SSAO-3
        // (cutsheet §S2 hard line).
        let ssaoSample = sample(ssaoTexture, uv)
        let aoFactor = clamp(ssaoSample.r, 0.0, 1.0)
        let aoGate = step(0.0001, ssaoStrength.x)
        let aoMul = clamp(1.0 - aoFactor * ssaoStrength.x * aoGate, 0.0, 1.0)
        let rawOccluded = rawHaze * aoMul
        let withBloom = rawOccluded + bloomSample.xyz * bloomStrength.x
        let cx = max(withBloom.x, 0.0)
        let cy = max(withBloom.y, 0.0)
        let cz = max(withBloom.z, 0.0)
        let rx = cx / (1.0 + cx)
        let ry = cy / (1.0 + cy)
        let rz = cz / (1.0 + cz)
        let ax = (cx * (2.51 * cx + 0.03)) / (cx * (2.43 * cx + 0.59) + 0.14)
        let ay = (cy * (2.51 * cy + 0.03)) / (cy * (2.43 * cy + 0.59) + 0.14)
        let az = (cz * (2.51 * cz + 0.03)) / (cz * (2.43 * cz + 0.59) + 0.14)
        let m = tonemapMode.x
        let selX = mix(mix(cx, rx, step(0.5, m)), ax, step(1.5, m))
        let selY = mix(mix(cy, ry, step(0.5, m)), ay, step(1.5, m))
        let selZ = mix(mix(cz, rz, step(0.5, m)), az, step(1.5, m))
        let mx = max(selX, 0.0)
        let my = max(selY, 0.0)
        let mz = max(selZ, 0.0)
        let invG = 1.0 / max(gammaParams.x, 0.0001)
        let encoded = vec3(pow(mx, invG), pow(my, invG), pow(mz, invG))
        return vec4(encoded, sampled.w)
    }
}
)";

constexpr const char* kPostProcessCacheKey = "postprocess_tonemap_aces_v6_prehazed_bloom_ssao_fs";

// Fallback if primary program fails to acquire — same tonemap+gamma
// contract so Editor composite does not go black / linear-washed.
constexpr const char* kPostProcessPassthroughSource = R"(
material PostProcessBlit {
    texture2d sceneColor
    texture2d bloomTexture
    texture2d hazeTexture
    texture2d ssaoTexture
    uniform vec4 bloomStrength
    uniform vec4 exposure
    uniform vec4 tonemapMode
    uniform vec4 uTime
    uniform vec4 gammaParams
    uniform vec4 hazeDensity
    uniform vec4 hazeStrength
    uniform vec4 hazeColor
    uniform vec4 ssaoStrength
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let sampled = sample(sceneColor, uv)
        let bloomSample = sample(bloomTexture, uv)
        // §S4c — sample the haze result RT0 (when bound by execute()
        // to ctx.depthHazePass->halfResFbo(); otherwise the sampler
        // returns the unbound sentinel = the same RT0 as sceneColor
        // since both share the BGRA8 layout). The composite uses
        // `hazeSample.w` (the haze FBO's alpha channel) as the dist
        // proxy — the DepthHazePass S4b FS writes the un-hazed raw
        // alpha there (`return vec4(mixed, raw.w)`), so
        // `exp(-density * 0) = 1` and `1 - 1 = 0` ⇒ fogFactor
        // collapses to 0 when the strength gate is off (K3 invariant
        // #3 branchless).
        let hazeSample = sample(hazeTexture, uv)
        let raw = sampled.xyz * exposure.x
        // §S4c fix (2026-07-23) — DepthHazePass already wrote the
        // exponential fog mix into hazeSample.rgb. Final PP must NOT
        // re-derive fog from alpha (raw.w was never a distance proxy).
        // Prefer the pre-hazed color when hazeStrength > 0; when the
        // haze slot falls back to sceneColor the mix is a no-op.
        let hazeWeight = step(0.0001, hazeStrength.x)
        let rawHaze = mix(raw, hazeSample.xyz * exposure.x, hazeWeight)
        // §A3 SSAO MVP (2026-07-24, mid-term FG MVP SSAO Gate) —
        // occlusion factor sample. SSAOPass writes R = occlusion
        // ∈ [0,1] onto SSAOTexture. Composite applies multiplicative
        // darkening `rawHaze * (1 - occlusion)`. When the SSAO slot
        // falls back to sceneColor (K-SSAO-1: ssaoEnabled=false ⇒
        // FG compile culls SSAOTexture ⇒ resolve returns invalid ⇒
        // sampler bound to sceneColor), the .r sample is NOT 1.0
        // (it's `raw.r * exposure.x`), so the `step(0.0001,
        // ssaoStrength.x)` strength gate MUST be the folding
        // mechanism, not the sampler. Phoskia has no `saturate`
        // builtin ⇒ collapse via `clamp(1 - x, 0, 1)` per K-SSAO-3
        // (cutsheet §S2 hard line).
        let ssaoSample = sample(ssaoTexture, uv)
        let aoFactor = clamp(ssaoSample.r, 0.0, 1.0)
        let aoGate = step(0.0001, ssaoStrength.x)
        let aoMul = clamp(1.0 - aoFactor * ssaoStrength.x * aoGate, 0.0, 1.0)
        let rawOccluded = rawHaze * aoMul
        let withBloom = rawOccluded + bloomSample.xyz * bloomStrength.x
        let cx = max(withBloom.x, 0.0)
        let cy = max(withBloom.y, 0.0)
        let cz = max(withBloom.z, 0.0)
        let rx = cx / (1.0 + cx)
        let ry = cy / (1.0 + cy)
        let rz = cz / (1.0 + cz)
        let ax = (cx * (2.51 * cx + 0.03)) / (cx * (2.43 * cx + 0.59) + 0.14)
        let ay = (cy * (2.51 * cy + 0.03)) / (cy * (2.43 * cy + 0.59) + 0.14)
        let az = (cz * (2.51 * cz + 0.03)) / (cz * (2.43 * cz + 0.59) + 0.14)
        let m = tonemapMode.x
        let selX = mix(mix(cx, rx, step(0.5, m)), ax, step(1.5, m))
        let selY = mix(mix(cy, ry, step(0.5, m)), ay, step(1.5, m))
        let selZ = mix(mix(cz, rz, step(0.5, m)), az, step(1.5, m))
        let mx = max(selX, 0.0)
        let my = max(selY, 0.0)
        let mz = max(selZ, 0.0)
        let invG = 1.0 / max(gammaParams.x, 0.0001)
        let encoded = vec3(pow(mx, invG), pow(my, invG), pow(mz, invG))
        return vec4(encoded, sampled.w)
    }
}
)";
constexpr const char* kPostProcessPassthroughCacheKey = "postprocess_passthrough_tonemap_aces_v6_prehazed_bloom_ssao_fs";

} // namespace

// §S4c (2026-07-23) — Bug fix #3 mirror (see DepthHazePass.cpp:118
// / BloomExtractPass.h / BloomBlurPass.h:185-199 for the originating
// pattern). Externalize the cache-key literal so unit tests can
// include PostProcessPass.h and compare their mirror against the
// live literal. Pre-S4c, kPostProcessCacheKey was a `.cpp` static
// (not addressable from outside), so tests would fall back to
// string self-comparison and drift detection would be a no-op
// (false green — same drift trap that bit Test_B5 in §P5.5 B).
// The extern declaration gives every test a single source of truth;
// drift = test fails immediately. S4c bumps the key to v4 (haze
// composite FS); future cuts bump to v5+.
//
// MUST live at file scope inside the `ayt::render::detail`
// namespace so the extern declaration in PostProcessPass.h finds it.
const char* const kPostProcessCacheKeyCStr = kPostProcessCacheKey;

uint32_t PostProcessPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;
    // Own blit view — must not reuse the scene view id (would clobber
    // scene FBO binding + camera VP for every FO submit that frame).
    // After BloomExtract=10 / BlurH=11 / BlurV=12 / DepthHaze=13;
    // before UI=255. Forward + Deferred share kBlitViewId.
    const uint8_t viewId = kBlitViewId;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;

    // R5+ — Noop-backend short-circuit. The headless test path runs
    // execute() against a default-constructed BGFXAdapter (isInitialized
    // == false). Every BGFXAdapter FBO method gates on isInitialized,
    // so we don't need a separate "if Noop skip" branch — but we DO
    // need an early-out here so we don't allocate any resources
    // (the FBO path inside ensureFbo would otherwise race against
    // bgfx::createFrameBuffer with no init context).
    if (!adapter.isInitialized()) {
        return 0;
    }
    if (adapter.isNoopBackend()) {
        // R5+ — the Noop backend returns valid handles for everything
        // (so handle-validity checks can't distinguish "real backend
        // that's broken" from "Noop that should skip"). The shutdown
        // path under Noop is also fragile — bgfx::destroy on Noop
        // handles in a partial state has caused flakiness in the
        // Test_CaptureScreenshot path. Skip the pass entirely here:
        // it preserves the P0 contract (post-process is opt-in via
        // FrameContext knobs; default values = no-op image).
        return 0;
    }

    if (viewportWidth == 0 || viewportHeight == 0) {
        return 0;
    }

    const uint16_t viewportX = ctx.viewportX;
    const uint16_t viewportY = ctx.viewportY;

    // §P5 B6 (2026-07-22) — source-FBO priority helper. Collapses
    // the cutsheet closure (pass-lessons-from-deferred.md:169)
    // and the P2 default into a single static. Priority:
    //   1. deferred path: ctx.gbufferPass->lightingOutputFbo()
    //      (only when LightingPass is mounted AND its FBO ensured)
    //   2. forward path: ctx.sceneFbo (P2 default)
    //   3. invalid → return 0 (no-op)
    //
    // Helper does NOT branch in execute() — same single linear flow
    // as before; only the sourceFbo value flips. No new view, no
    // new shader, no new uniform path.
    const bgfx::FrameBufferHandle sourceFbo = selectSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        return 0;
    }

    ensureFullscreenQuad(adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }
    ensureProgram(pool);
    // §S1c (2026-07-23) — added _tBloomTexture to the ready check.
    // When the program acquires successfully (kPostProcessCacheKey
    // bumped to v3_bloom_composite_fs), the Phoskia source declares
    // `texture2d bloomTexture` and the binding resolves to non-zero.
    // §S4c (2026-07-23) — added _tHazeTexture + _uHazeDensity +
    // _uHazeStrength + _uHazeColor to the ready check. v4 cache-key
    // bump forces Phoskia to re-acquire; the hazeTexture sampler +
    // 3 vec4 uniforms resolve to non-zero on success. Phoskia parser
    // failure path leaves _program invalid → the whole `programReady`
    // expression short-circuits to false and execute() returns 0
    // (mirror R5.1 fallback contract).
    const bool programReady = _program.isValid()
        && _uBloomStrength != ayt::shader::InvalidBinding
        && _uExposure      != ayt::shader::InvalidBinding
        && _uTonemapMode   != ayt::shader::InvalidBinding
        && _uTime          != ayt::shader::InvalidBinding
        && _uGammaParams   != ayt::shader::InvalidBinding
        && _uHazeDensity   != ayt::shader::InvalidBinding
        && _uHazeStrength  != ayt::shader::InvalidBinding
        && _uHazeColor     != ayt::shader::InvalidBinding
        && _uSSAOStrength  != ayt::shader::InvalidBinding
        && _tSceneColor    != ayt::shader::InvalidBinding
        && _tBloomTexture  != ayt::shader::InvalidBinding
        && _tHazeTexture   != ayt::shader::InvalidBinding
        && _tSSAOTexture   != ayt::shader::InvalidBinding;

    const bgfx::TextureHandle fboColor = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(fboColor)) {
        return 0;
    }

    // Single blit: sample source color → default backbuffer.
    // Do NOT bind sourceFbo as the draw target while sampling it
    // (same-FBO feedback clears/blacks the Editor viewport).
    //
    // View rect uses the editor panel offset (vx,vy) so the blit
    // lands in the Game View hole, not at the window origin.
    // Identity view/proj: the fullscreen triangle is already in NDC;
    // leaving the camera matrices from FO would warp it off-screen.
    const ayt::math::Float4x4 identity = ayt::math::Float4x4::identity();
    adapter.setViewFrameBuffer(viewId, BGFX_INVALID_HANDLE);
    adapter.setViewRect(viewId, viewportX, viewportY, viewportWidth, viewportHeight);
    adapter.setViewTransform(viewId, identity, identity);
    adapter.setViewClearRaw(viewId, BGFX_CLEAR_NONE, 0, 1.0f, 0);

    if (!programReady) {
        static bool s_loggedMissing = false;
        if (!s_loggedMissing) {
            std::fprintf(stderr,
                "[PostProcessPass] FATAL: no blit program — scene stays in FBO "
                "(Game View may be black). Check Phoskia acquire errors above.\n");
            s_loggedMissing = true;
        }
        return 0;
    }

    const ayt::shader::TextureHandle texHandle =
        ayt::render::detail::toShaderTexture(fboColor);
    // §F5 (2026-07-24, mid-term FG MVP sub-cut 5) — second sampler
    // (bloomTexture). Read via `ctx.frameGraph->resolveSemantic(
    // FgSemantic::BloomSource)`, replacing the pre-F5
    // `ctx.bloomBlurPass->pongFbo()` producer-pointer pattern. The
    // FG's semantic layer is the single source of truth for which
    // physical RT feeds the bloom sampler; render() central wiring
    // resolves BloomSource → BloomBlurB (when bloomEnabled) →
    // invalid fallback to sceneColor. Falls back to sceneColor
    // (sentinel) when the semantic physical is invalid — same
    // shape as the pre-F5 `pongFbo() invalid ⇒ fallback` pattern.
    ayt::shader::TextureHandle bloomTexHandle = texHandle;  // fallback
    bgfx::FrameBufferHandle bloomSourceFbo = BGFX_INVALID_HANDLE;
    if (ctx.frameGraph != nullptr) {
        bloomSourceFbo = ctx.frameGraph->resolveSemantic(
            ayt::render::detail::FgSemantic::BloomSource);
    }
    if (BGFXAdapter::isValid(bloomSourceFbo)) {
        const bgfx::TextureHandle bloomColor =
            adapter.getFboAttachment(bloomSourceFbo, 0);
        if (BGFXAdapter::isValid(bloomColor)) {
            bloomTexHandle =
                ayt::render::detail::toShaderTexture(bloomColor);
        }
    }
    // bgfx Vec4 slots — pad scalars into .x (lessons §3.1).
    const float bloomPad[4] = {frame.bloomStrength, 0.0f, 0.0f, 0.0f};
    const float exposurePad[4] = {frame.exposure, 0.0f, 0.0f, 0.0f};
    const float tonemapPad[4] = {
        static_cast<float>(static_cast<int32_t>(frame.tonemapMode)), 0.0f, 0.0f, 0.0f};
    const float timePad[4] = {frame.timeSeconds, 0.0f, 0.0f, 0.0f};
    const float gammaPad[4] = {frame.gamma, 0.0f, 0.0f, 0.0f};
    // §S4c (2026-07-23) — three new vec4 uniforms for the haze
    // composite (hazeDensity / hazeStrength / hazeColor). Mirror
    // DepthHazePass S4b upload shape — same knobs, same per-frame
    // FrameContext source — so the strength gate is consistent across
    // the half-res FS write (DepthHazePass) and the full-res FS
    // composite (PostProcessPass). hazeColor carries .xyz, .w zero pad.
    const float hazeDensityPad[4]  = {frame.hazeDensity, 0.0f, 0.0f, 0.0f};
    const float hazeStrengthPad[4] = {frame.hazeStrength, 0.0f, 0.0f, 0.0f};
    const float hazeColorPad[4]    = {
        frame.hazeColor.x, frame.hazeColor.y, frame.hazeColor.z, 0.0f};

    adapter.setTransformIdentity();
    adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    adapter.setIndexBuffer(_fullscreenIB, 0, 3);
    // Bind by recorded SAMPLER2D slots (pass stage=0 so setTexture does
    // not override compile-time units — see AYShaderResource.h).
    _program.setTexture(0, _tSceneColor, texHandle);
    _program.setTexture(0, _tBloomTexture, bloomTexHandle);
    // §F5 (2026-07-24) — third sampler now read through
    // `ctx.frameGraph->resolveSemantic(FgSemantic::HazeSource)`,
    // replacing the pre-F5 `ctx.depthHazePass->halfResFbo()` shape.
    // FG's semantic layer is the single source of truth; render()
    // central wiring resolves HazeSource → HazeHalf (when
    // hazePassEnabled) → invalid fallback to sceneColor. Same
    // fallback contract as S4c: invalid ⇒ sceneColor on the slot
    // ⇒ branchless strength gate collapses haze mix to 0
    // (K3 invariant #3).
    ayt::shader::TextureHandle hazeTexHandle = texHandle;  // fallback
    bgfx::FrameBufferHandle hazeSourceFbo = BGFX_INVALID_HANDLE;
    if (ctx.frameGraph != nullptr) {
        hazeSourceFbo = ctx.frameGraph->resolveSemantic(
            ayt::render::detail::FgSemantic::HazeSource);
    }
    if (BGFXAdapter::isValid(hazeSourceFbo)) {
        const bgfx::TextureHandle hazeRtColor =
            adapter.getFboAttachment(hazeSourceFbo, 0);
        if (BGFXAdapter::isValid(hazeRtColor)) {
            hazeTexHandle =
                ayt::render::detail::toShaderTexture(hazeRtColor);
        }
    }
    _program.setTexture(0, _tHazeTexture, hazeTexHandle);
    // §A3 SSAO MVP (2026-07-24, mid-term FG MVP SSAO Gate) —
    // fourth sampler on the fullscreen-triangle composite draw.
    // Read via `ctx.frameGraph->resolveSemantic(
    // FgSemantic::SSAOSource)` — mirror of the haze/bloom pattern
    // (F5-shipped). When SSAOPass is mounted AND enabled AND the
    // render() central ssaoPassEnabled gate is true, the SSAOTexture
    // is live and `resolveSemantic` returns the physical handle.
    // Otherwise the FG compile culled SSAOTexture ⇒ invalid ⇒
    // fallback to `texHandle` (sceneColor); the FS gate
    // `step(0.0001, ssaoStrength.x)` collapses the AO contribution
    // to zero (K-SSAO-1 + K-SSAO-3 hold).
    ayt::shader::TextureHandle ssaoTexHandle = texHandle;  // fallback
    bgfx::FrameBufferHandle ssaoSourceFbo = BGFX_INVALID_HANDLE;
    if (ctx.frameGraph != nullptr) {
        ssaoSourceFbo = ctx.frameGraph->resolveSemantic(
            ayt::render::detail::FgSemantic::SSAOSource);
    }
    if (BGFXAdapter::isValid(ssaoSourceFbo)) {
        const bgfx::TextureHandle ssaoRtColor =
            adapter.getFboAttachment(ssaoSourceFbo, 0);
        if (BGFXAdapter::isValid(ssaoRtColor)) {
            ssaoTexHandle =
                ayt::render::detail::toShaderTexture(ssaoRtColor);
        }
    }
    _program.setTexture(0, _tSSAOTexture, ssaoTexHandle);
    _program.setUniform(_uBloomStrength, bloomPad, sizeof(bloomPad));
    _program.setUniform(_uExposure, exposurePad, sizeof(exposurePad));
    _program.setUniform(_uTonemapMode, tonemapPad, sizeof(tonemapPad));
    _program.setUniform(_uTime, timePad, sizeof(timePad));
    _program.setUniform(_uGammaParams, gammaPad, sizeof(gammaPad));
    _program.setUniform(_uHazeDensity, hazeDensityPad, sizeof(hazeDensityPad));
    _program.setUniform(_uHazeStrength, hazeStrengthPad, sizeof(hazeStrengthPad));
    _program.setUniform(_uHazeColor, hazeColorPad, sizeof(hazeColorPad));
    // §A3 SSAO MVP (2026-07-24) — ssaoStrength vec4 uniform on the
    // strength gate. Background fill (ssao enabled but slot fallback)
    // ⇒ 0 — FS gate collapses. Mirror haze-strength pattern.
    const float ssaoStrengthPad[4] = {frame.ssaoStrength, 0.0f, 0.0f, 0.0f};
    _program.setUniform(_uSSAOStrength, ssaoStrengthPad, sizeof(ssaoStrengthPad));

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;  // P6.5: per-draw state owned by Adapter (see
                       // setStateDepthTestAlways() called below).
    // P6.5 (2026-07-22) — preset state replaces the inline
    // `BGFX_STATE_WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS`. Bit
    // combination identical.
    adapter.setStateDepthTestAlways();
    _program.submit(sub);

    static bool s_loggedSubmit = false;
    if (!s_loggedSubmit) {
        const bool bloomFromPong =
            BGFXAdapter::isValid(bloomSourceFbo)
            && (bloomTexHandle.id != texHandle.id);
        // §F5 (2026-07-24) — haze slot log mirror shape: "hazeFromHalf"
        // = the haze tex came from FG.resolveSemantic(HazeSource)
        // and that physical differs from sceneColor; otherwise
        // it's the fallback (sceneColor) and the FS haze mix
        // collapses to raw via the branchless strength gate.
        const bool hazeFromHalf =
            BGFXAdapter::isValid(hazeSourceFbo)
            && (hazeTexHandle.id != texHandle.id);
        std::fprintf(stderr,
            "[PostProcessPass] blit ok view=%u rect=(%u,%u,%u,%u) "
            "gamma=%.1f exposure=%.2f tonemap=%.0f bloom=%.2f "
            "bloomSrc=%s haze=%.2f density=%.3f hazeSrc=%s time=%.2f\n",
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(viewportX),
            static_cast<unsigned>(viewportY),
            static_cast<unsigned>(viewportWidth),
            static_cast<unsigned>(viewportHeight),
            gammaPad[0],
            frame.exposure,
            tonemapPad[0],
            frame.bloomStrength,
            bloomFromPong ? "pong" : "fallback(scene)",
            frame.hazeStrength,
            frame.hazeDensity,
            hazeFromHalf ? "hazeHalf" : "fallback(scene)",
            frame.timeSeconds);
        s_loggedSubmit = true;
    }
    return 1;
}

void PostProcessPass::ensureFbo(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    if (BGFXAdapter::isValid(_fbo) && _fboWidth == width && _fboHeight == height) {
        return;
    }

    // R5+ — size changed (or first call): destroy the old FBO and
    // recreate at the new dimensions. BGFXAdapter handles the destroy
    // gracefully when the handle is invalid.
    if (BGFXAdapter::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo = BGFX_INVALID_HANDLE;
    }

    _fbo = adapter.createFrameBuffer(width, height,
                                      bgfx::TextureFormat::RGBA8,
                                      /*withDepth=*/true);
    if (BGFXAdapter::isValid(_fbo)) {
        _fboWidth  = width;
        _fboHeight = height;
    } else {
        _fboWidth  = 0;
        _fboHeight = 0;
        std::fprintf(stderr,
                     "[PostProcessPass] FBO create failed at %ux%u; "
                     "post-process disabled for this run\n",
                     width, height);
    }
}

void PostProcessPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }

    // R5+ (Pass-side backfill) — funnel the VB/IB creation through
    // BGFXAdapter. Layout MUST match FullscreenVertex {x,y,u,v}:
    // an empty `bgfx::VertexLayout{}` is invalid (stride 0) and
    // triggers bgfx::fatal / debugBreak under Debug builds the first
    // time PostProcess runs on a real GPU backend. TexCoord0 is
    // packed for stride alignment; the Phoskia VS currently rebuilds
    // UV from pos.xy (see kPostProcessPhoskiaSource).
    //
    // P6.5 (2026-07-22) — layout construction now goes through
    // BGFXAdapter::vertexLayoutPosUv() instead of inlining
    // bgfx::VertexLayout::begin().add(...).end() here. The
    // returned layout has the same byte shape (Position 2 floats +
    // TexCoord0 2 floats).
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();

    _fullscreenVB = adapter.createVertexBuffer(kFullscreenTriangle,
                                                sizeof(kFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kFullscreenIndices,
                                              sizeof(kFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void PostProcessPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    // Cache-key bump forces re-acquire (pointer-equal compare).
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kPostProcessCacheKey) {
        _program.reset();
        _programAcquireFailed = false;
        s_acquiredCacheKey = kPostProcessCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }
    // Prefer tonemap+gamma blit; fall back to identical passthrough
    // so Editor composite (FBO → backbuffer) never goes black.
    ayt::shader::ShaderResource acquired =
        pool.acquire(kPostProcessPhoskiaSource, kPostProcessCacheKey);
    if (!acquired.isValid()) {
        std::fprintf(stderr,
                     "[PostProcessPass] tonemap Phoskia acquire failed; "
                     "trying passthrough blit\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[PostProcessPass]   %s\n", err.c_str());
        }
        acquired = pool.acquire(kPostProcessPassthroughSource,
                                kPostProcessPassthroughCacheKey);
    }
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[PostProcessPass] Phoskia acquire failed; "
                     "post-process will skip blit (scene may stay offscreen)\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[PostProcessPass]   %s\n", err.c_str());
        }
        return;
    }
    _program        = acquired;
    _uBloomStrength = _program.getUniformBinding("bloomStrength");
    _uExposure      = _program.getUniformBinding("exposure");
    _uTonemapMode   = _program.getUniformBinding("tonemapMode");
    _uTime          = _program.getUniformBinding("uTime");
    _uGammaParams   = _program.getUniformBinding("gammaParams");
    // §S4c (2026-07-23) — three new vec4 uniforms for the haze
    // composite. Phoskia source declares `uniform vec4 hazeDensity`
    // / hazeStrength / hazeColor; binding names match what
    // `execute()` uploads via setUniform. Resolution returns
    // InvalidBinding when the program couldn't be acquired (mirror
    // _uBloomStrength — gated on _program.isValid()).
    _uHazeDensity   = _program.getUniformBinding("hazeDensity");
    _uHazeStrength  = _program.getUniformBinding("hazeStrength");
    _uHazeColor     = _program.getUniformBinding("hazeColor");
    _tSceneColor    = _program.getTextureBinding("sceneColor");
    // §S1c (2026-07-23) — second sampler for the true bloom composite.
    // Phoskia source declares `texture2d bloomTexture`; binding name
    // matches what `execute()` uploads at slot 1. Resolution returns
    // InvalidBinding when the program couldn't be acquired (mirror
    // _tSceneColor — gated on _program.isValid()).
    _tBloomTexture  = _program.getTextureBinding("bloomTexture");
    // §S4c (2026-07-23) — third sampler for the haze composite.
    // Phoskia source declares `texture2d hazeTexture`; binding name
    // matches what `execute()` uploads at slot 2 (after
    // ctx.depthHazePass->halfResFbo() RT0, or sceneColor fallback).
    _tHazeTexture   = _program.getTextureBinding("hazeTexture");
    // §A3 SSAO MVP (2026-07-24) — fourth sampler for the SSAO
    // composite. Phoskia source declares `texture2d ssaoTexture`
    // (v6 cache-key bump forces re-acquire); execute() uploads at
    // slot 3 from `ctx.frameGraph->resolveSemantic(FgSemantic::
    // SSAOSource)` RT0, or sceneColor fallback when FG culled
    // SSAOTexture (K-SSAO-1 hold).
    _tSSAOTexture   = _program.getTextureBinding("ssaoTexture");
    // §A3 SSAO MVP (2026-07-24) — vec4 uniform for the SSAO strength
    // gate. .x carries strength; the FS gates `step(0.0001, .x)` so a
    // 0 strength produces a no-op AO contribution (K-SSAO-3).
    _uSSAOStrength  = _program.getUniformBinding("ssaoStrength");
}

void PostProcessPass::destroyResources(BGFXAdapter& adapter)
{
    if (BGFXAdapter::isValid(_fbo)) {
        adapter.destroy(_fbo);
        _fbo = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = BGFX_INVALID_HANDLE;
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = BGFX_INVALID_HANDLE;
    }
    if (_program.isValid()) {
        // R5.1 — release the ShaderResource. ShaderResource doesn't
        // carry a back-pointer to its pool; the pool reference is
        // held by Renderer::Impl and outlives this pass (see
        // AYRenderer.cpp:160 shutdown order: resources → shaderPool
        // → adapter). Resetting _program here is the documented
        // ShaderResource contract (see ShaderResource::reset in
        // AYShaderResource.h:32) — the pool's dtor releases the
        // underlying GPU program once the refcount hits zero,
        // regardless of which ShaderResource instance held the last
        // reference.
        _program.reset();
    }
    _uBloomStrength = ayt::shader::InvalidBinding;
    _uExposure      = ayt::shader::InvalidBinding;
    _uTonemapMode   = ayt::shader::InvalidBinding;
    _uTime          = ayt::shader::InvalidBinding;
    _uGammaParams   = ayt::shader::InvalidBinding;
    // §S4c (2026-07-23) — reset the three new vec4 haze uniforms
    // + the hazeTexture sampler binding so a stale resolved id from
    // the previous program acquire doesn't poison the next acquire
    // (mirror _uBloomStrength reset above).
    _uHazeDensity   = ayt::shader::InvalidBinding;
    _uHazeStrength  = ayt::shader::InvalidBinding;
    _uHazeColor     = ayt::shader::InvalidBinding;
    _tSceneColor    = ayt::shader::InvalidBinding;
    _tBloomTexture  = ayt::shader::InvalidBinding;
    _tHazeTexture   = ayt::shader::InvalidBinding;
    // §A3 SSAO MVP (2026-07-24) — reset the new vec4 SSAO uniform
    // + the ssaoTexture sampler binding so a stale resolved id from
    // the previous program acquire doesn't poison the next acquire
    // (mirror _uBloomStrength reset above).
    _uSSAOStrength  = ayt::shader::InvalidBinding;
    _tSSAOTexture   = ayt::shader::InvalidBinding;
    _programAcquireFailed = false;
    _fboWidth = 0;
    _fboHeight = 0;
}

// §P5 B6 (2026-07-22) — source-FBO priority helper. See
// PostProcessPass.h for the priority ordering. Static, not a
// member of PostProcessPass, so tests can call it directly
// without spinning up execute() / ensure path.
//
// §F5 (2026-07-24, mid-term FG MVP sub-cut 5) — internal
// resolution switches to `ctx.frameGraph->resolveSemantic(
// FgSemantic::FinalColorSource)` first; the FG compile step
// has already decided whether FinalColorSource points at
// LightingPass::lightingOutputFbo() (Deferred path), the
// Renderer's sceneFbo (Forward path), or invalid. Pre-F5
// priority order is preserved through the FG's compile-time
// resolve (the FG itself walks the B6 priority via the
// `gbufferPass` / `lightingPass` borrowed pointers at the
// resolveSemantic call site). Falls back to the pre-F5
// inspection of `ctx.sceneFbo` when `ctx.frameGraph == nullptr`
// so legacy test sites still work (C++14 trailing-default for
// frameGraph field).
bgfx::FrameBufferHandle PostProcessPass::selectSourceFbo(
    const PassExecContext& ctx) noexcept
{
    // 1) F5 — FinalColorSource semantic from the FrameGraph.
    //    When the frameGraph is wired, the FG compile step
    //    already resolved FinalColorSource to a physical handle
    //    (Deferred ⇒ LightingOutput; Forward ⇒ sceneFbo). Returns
    //    invalid when neither is available so the caller can
    //    short-circuit.
    if (ctx.frameGraph != nullptr) {
        const bgfx::FrameBufferHandle fgSource = ctx.frameGraph->resolveSemantic(
            ayt::render::detail::FgSemantic::FinalColorSource);
        if (BGFXAdapter::isValid(fgSource)) {
            return fgSource;
        }
    }

    // 2) Legacy fallback (frameGraph not wired OR FG returned
    //    invalid) — the B6 priority walk as documented pre-F5:
    //    Deferred's LightingOutputFbo (when LightingPass is
    //    mounted AND its ensure ran this frame), else the P2
    //    sceneFbo, else invalid. Keeps the cut-1 bisect / §5.3
    //    test sites compiling without edits.
    if (ctx.gbufferPass != nullptr && ctx.lightingPass != nullptr) {
        const bgfx::FrameBufferHandle lightingFbo =
            ctx.lightingPass->lightingOutputFbo();
        if (bgfx::isValid(lightingFbo)) {
            return lightingFbo;
        }
    }

    if (BGFXAdapter::isValid(ctx.sceneFbo)) {
        return ctx.sceneFbo;
    }

    return BGFX_INVALID_HANDLE;
}

} // namespace ayt::render::detail
