#include "detail/LightingPass.h"

#include "detail/BgfxMatrix.h"
#include "detail/FrameContext.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/RenderPass.h"
#include "detail/ShadowPass.h"

#include "AYRenderScene.h"

#include <cstdio>

namespace ayt::render::detail
{

// §P5 B5 (2026-07-22) — build stamp literal (mirror
// GBufferPass.cpp:15 `kGBufferBuildStamp`). Pointer-equal compare;
// bumping this triggers a FBO rebuild on next execute(). B5 is
// the first lock — bumping is safe across B5.x cuts.
static constexpr const char* kLightingBuildStamp = "b5-2026-07-22";

// §P5 B5 (2026-07-22) — cache key literal (mirror GBufferPass.cpp:68
// `kGBufferCacheKey`). Pointer-equal compare so cache invalidates
// when the literal bumps. `s_acquiredCacheKey` static guard inside
// ensureProgram() forces re-acquire.
// §P5 B7+ (2026-07-22) — bump to v4: Phoskia source now declares a
// `uniformblock Lights { vec4 dirs[8]; vec4 colors[8]; } binding 0`
// and the FS unrolls 8 directional-light taps unconditionally.
// Single-light fallback path through u_lightDirection +
// u_lightColor still exists (no-op on the data path; both uniforms
// still ship) so existing setDirectionalLight host code stays
// compatible.
// §P5 B7+ (2026-07-22) — v6: Lights UBO must be program-top-level.
// AYBGFXConverter only emits cbuffer from IRProgram::uniformBlocks;
// nested material uniformblocks never reach the HLSL preamble
// (`undeclared identifier 'Lights'` / D3D X3004).
// §P5 B5.5 (2026-07-22) — bump on top of v11:
//   + `texture2d gbufferDepth` (sampled into the FS for worldPos
//     reconstruction)
//   + `texture2d shadowMap` + 4 shadow uniforms (u_lightViewProj,
//     shadowBias, shadowMapTexel, shadowPcf) plus two inverse-view
//     uniforms (u_invView, u_invProjection) for worldPos
//     reconstruction
//   + FS key-light only PCF (9 taps) — fill / rim lights
//     (lights[1..7]) stay unshadowed.
//
// Single shared shadow map for the key light (cutsheet §10
// pass-lessons-from-deferred.md:300 — "LightingPass 共用
// ShadowPass 只读借用"). Per-light shadow maps is a separate
// future cut. v0 keeps the existing `lighting_v11_b7_ubo_struct_
// types` base + suffix `+b5p5_worldpos_pcf_key_only`.
static constexpr const char* kLightingCacheKey = "lighting_v11_b7_ubo_struct_types_b5p5_worldpos_pcf_key_only";

// §P5 B5 (2026-07-22) — fullscreen-triangle vertex data, duplicated
// from PostProcessPass.cpp:27-33 (private state there — coupling
// would require a friend class, duplicate is cheaper). Same
// 3-vert NDC oversize-triangle bgfx pattern; same FullscreenVertex
// {x,y,u,v} layout that vertexLayoutPosUv() emits.
struct alignas(16) LightingFullscreenVertex {
    float x;
    float y;
    float u;
    float v;
};

constexpr LightingFullscreenVertex kLightingFullscreenTriangle[3] = {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    {  3.0f, -1.0f, 2.0f, 1.0f },
    { -1.0f,  3.0f, 0.0f, -1.0f },
};

constexpr uint16_t kLightingFullscreenIndices[3] = { 0, 1, 2 };

// §P5 B5 (2026-07-22) — Phoskia Lighting VS/FS source.
//
// Sampler inputs (cutsheet B5 spec):
//   - gbufferAlbedo : color (RGB = baseColor, A = opacity) from B4b
//   - gbufferNormal : color (RGB = world-space normal encoded [0,1])
//   - gbufferMotion : color (xy = prev-frame displacement, [0,1] —
//                                   B5 currently does NOT consume
//                                   motion; binding present for B7+
//                                   TAA consumer symmetry — cutsheet
//                                   B5 boundary documented)
//
// Uniform inputs (cutsheet B5 spec, all from FrameContext):
//   - u_lightDirection : vec3 = -frame.lightDirection (Phoskia wants
//                                          a TO-light vector; we
//                                          negate on the CPU once,
//                                          feed as-is)
//   - u_lightColor     : vec3 = frame.lightColor
//   - u_cameraPos      : vec3 = frame.cameraPosition
//     (reserved for B5.5 specular/Blinn-Phong — B5 keeps Lambert
//     flat; uploader still emits it so future B5.x can reuse the
//     same uniform binding without shader changes)
//
// Math (Lambert):
//   N = sample(gbufferNormal).xyz * 2.0 - vec3(1,1,1)  // decode [0,1]→[-1,1]
//   L = normalize(u_lightDirection)
//   NdotL = max(dot(N, L), 0.0)
//   ambient = vec3(0.1)                                  // floor term
//   lit = sample(gbufferAlbedo).rgb * (ambient + NdotL * u_lightColor)
//   return vec4(lit, sample(gbufferAlbedo).a)
//
// Phoskia `let` chain uses the same surface as the B4b GBufferFill
// source (verified at PR-F2/PR-F3 ship) — `let` declarations +
// arithmetic + sample() texture lookups. B5 emits a SINGLE `return`
// (not MRT) so no Phoskia MRT extension needed — falls back to the
// legacy `return → gl_FragColor` path (verified at AYShader/
// unittest/Test_MRT_Fragment.cpp::mrt_legacy_return_still_emits_fragcolor).
constexpr const char* kLightingPhoskiaSource = R"(
uniformblock Lights {
    vec4 dirs[8]
    vec4 colors[8]
} binding 0
material Lighting {
    texture2d gbufferAlbedo
    texture2d gbufferNormal
    texture2d gbufferMotion
    texture2d gbufferDepth
    texture2d shadowMap
    uniform vec4 u_lightDirection
    uniform vec4 u_lightColor
    uniform vec4 u_cameraPos
    uniform mat4 u_invView
    uniform mat4 u_invProjection
    uniform mat4 u_lightViewProj
    uniform vec4 shadowBias
    uniform vec4 shadowMapTexel
    uniform vec4 shadowPcf
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        let baseUv = vec2(vUv.x, 1.0 - vUv.y)
        let albedo = sample(gbufferAlbedo, baseUv)
        let normalSample = sample(gbufferNormal, baseUv)
        let N = normalSample.xyz * 2.0 - vec3(1.0, 1.0, 1.0)
        let ambient = vec3(0.1, 0.1, 0.1)
        // Empty slots are zero dirs — never normalize(0) (D3D hangs / NaN).
        let L0 = Lights.dirs[0].xyz * (1.0 / max(length(Lights.dirs[0].xyz), 0.0001))
        let L1 = Lights.dirs[1].xyz * (1.0 / max(length(Lights.dirs[1].xyz), 0.0001))
        let L2 = Lights.dirs[2].xyz * (1.0 / max(length(Lights.dirs[2].xyz), 0.0001))
        let L3 = Lights.dirs[3].xyz * (1.0 / max(length(Lights.dirs[3].xyz), 0.0001))
        let L4 = Lights.dirs[4].xyz * (1.0 / max(length(Lights.dirs[4].xyz), 0.0001))
        let L5 = Lights.dirs[5].xyz * (1.0 / max(length(Lights.dirs[5].xyz), 0.0001))
        let L6 = Lights.dirs[6].xyz * (1.0 / max(length(Lights.dirs[6].xyz), 0.0001))
        let L7 = Lights.dirs[7].xyz * (1.0 / max(length(Lights.dirs[7].xyz), 0.0001))
        let f0 = max(dot(N, L0), 0.0)
        let f1 = max(dot(N, L1), 0.0)
        let f2 = max(dot(N, L2), 0.0)
        let f3 = max(dot(N, L3), 0.0)
        let f4 = max(dot(N, L4), 0.0)
        let f5 = max(dot(N, L5), 0.0)
        let f6 = max(dot(N, L6), 0.0)
        let f7 = max(dot(N, L7), 0.0)
        // §P5 B5.5 (2026-07-22) — Shadow attenuation for the **key
        // light** only (Lights.dirs[0] / colors[0], shared with the
        // single ShadowPass producer — cutsheet pass-lessons-from-
        // deferred.md:300 "LightingPass 共用 ShadowPass 只读借用").
        // Fill / rim lights (Lights.dirs[1..7]) get NO shadow
        // attenuation — they don't carry their own shadow map in
        // v0. Per-light shadow maps is a separate cut.
        //
        // Wire (mirrors AYShadowShaderSources.h simple_lit_shadow
        // PCF contract):
        //   1. Sample gbufferDepth at baseUv → linearized ndcZ
        //      ∈ [0, 1]. tex = sample(gbufferDepth, baseUv).x.
        //      (GBufferPass RT3 is D24S8 hw depth; bgfx exposes it
        //      as a depth texture that shaders can sample as
        //      `.x`-linearized in [0,1] convention. We keep this
        //      simple by treating .x as a direct ndc01 read; the
        //      exact half-range decode is host-tunable once VSM
        //      lands.)
        //   2. Reconstruct world position via inverse-projection →
        //      inverse-view chain (MathInput `u_invProjection` /
        //      `u_invView` uniforms; both derived per-frame in
        //      execute() via Float4x4::inverse — FrameContext 0
        //      changed, inverse lives on the pass only).
        //   3. Project worldPos through u_lightViewProj → ndc01.
        //      9-tap PCF compare against gbufferDepth-derived
        //      refNdc01; the 3×3 kernel + 1/9 mixer + step(0.999,
        //      o) sky-bypass matches simple_lit_shadow.phoskia.
        let texDepth = sample(gbufferDepth, baseUv).x
        // Inverse projection: clip → view. ndc = (x, y, ndcZ*2-1,
        // 1). Split-half v0: ndcY flip already done in baseUv.
        let ndc01 = vec3(baseUv.x * 2.0 - 1.0, baseUv.y * 2.0 - 1.0, texDepth * 2.0 - 1.0)
        let viewPos4 = u_invProjection * vec4(ndc01, 1.0)
        let viewPos = viewPos4.xyz / max(viewPos4.w, 0.0001)
        let worldPos4 = u_invView * vec4(viewPos, 1.0)
        let worldPos = worldPos4.xyz
        // Project into shadow space.
        let clipPos = u_lightViewProj * vec4(worldPos, 1.0)
        let invW = 1.0 / max(clipPos.w, 0.0001)
        let ndcX = clipPos.x * invW
        let ndcY = clipPos.y * invW
        let refNdc01 = clipPos.z * invW
        let uy = ndcY * 0.5 + 0.5
        let shadowUv = vec2(ndcX * 0.5 + 0.5, 1.0 - uy)
        let inMap = step(0.0, shadowUv.x) * step(shadowUv.x, 1.0)
                   * step(0.0, shadowUv.y) * step(shadowUv.y, 1.0)
        let tx = shadowMapTexel.x
        let ty = shadowMapTexel.y
        let biasVal = shadowBias.x
        let o00 = sample(shadowMap, shadowUv + vec2(-tx, -ty)).x
        let o10 = sample(shadowMap, shadowUv + vec2(0.0, -ty)).x
        let o20 = sample(shadowMap, shadowUv + vec2(tx, -ty)).x
        let o01 = sample(shadowMap, shadowUv + vec2(-tx, 0.0)).x
        let o11 = sample(shadowMap, shadowUv + vec2(0.0, 0.0)).x
        let o21 = sample(shadowMap, shadowUv + vec2(tx, 0.0)).x
        let o02 = sample(shadowMap, shadowUv + vec2(-tx, ty)).x
        let o12 = sample(shadowMap, shadowUv + vec2(0.0, ty)).x
        let o22 = sample(shadowMap, shadowUv + vec2(tx, ty)).x
        let s00 = max(1.0 - step(o00 + biasVal, refNdc01), step(0.999, o00))
        let s10 = max(1.0 - step(o10 + biasVal, refNdc01), step(0.999, o10))
        let s20 = max(1.0 - step(o20 + biasVal, refNdc01), step(0.999, o20))
        let s01 = max(1.0 - step(o01 + biasVal, refNdc01), step(0.999, o01))
        let s11 = max(1.0 - step(o11 + biasVal, refNdc01), step(0.999, o11))
        let s21 = max(1.0 - step(o21 + biasVal, refNdc01), step(0.999, o21))
        let s02 = max(1.0 - step(o02 + biasVal, refNdc01), step(0.999, o02))
        let s12 = max(1.0 - step(o12 + biasVal, refNdc01), step(0.999, o12))
        let s22 = max(1.0 - step(o22 + biasVal, refNdc01), step(0.999, o22))
        let shadowSoft = (s00 + s10 + s20
                        + s01 + s11 + s21
                        + s02 + s12 + s22) * (1.0 / 9.0)
        let shadowHard = s11
        let shadowFilt = mix(shadowHard, shadowSoft, shadowPcf.x)
        let shadowKey = mix(1.0, shadowFilt, inMap)
        // Key-light multiplication: only lights[0] gets the shadow
        // attenuation. Fill / rim lights (1..7) stay unshadowed.
        let keyContribution = f0 * shadowKey * Lights.colors[0].xyz
        let fillContribution = f1 * Lights.colors[1].xyz
                            + f2 * Lights.colors[2].xyz
                            + f3 * Lights.colors[3].xyz
                            + f4 * Lights.colors[4].xyz
                            + f5 * Lights.colors[5].xyz
                            + f6 * Lights.colors[6].xyz
                            + f7 * Lights.colors[7].xyz
        let directionalSum = keyContribution + fillContribution
        let lit = albedo.rgb * (ambient + directionalSum)
        return vec4(lit, albedo.a)
    }
}
)";

LightingPass::~LightingPass() = default;

void LightingPass::setOutputSize(uint16_t width, uint16_t height) noexcept
{
    // §P5 B5 (2026-07-22) — host-driven store-only call (mirror
    // GBufferPass::setGbufferSize at GBufferPass.cpp:231-238).
    // No adapter access here; the next execute() honors the size.
    _lightingW = width;
    _lightingH = height;
}

void LightingPass::destroyResources(BGFXAdapter& adapter)
{
    // §P5 B5 (2026-07-22) — mirror GBufferPass::destroyResources at
    // GBufferPass.cpp:240-266. Drop the FBO, fullscreen VB/IB, and
    // reset all cached handles. W/H + buildStamp reset UNCONDITIONALLY
    // so a host that calls setOutputSize(800,600) → destroyResources()
    // expects W/H back to 0 (Test_B5 case 6 pins this). The FBO/VB/IB
    // handles are only destroyed when actually allocated (calling
    // bgfx::destroy on an invalid handle is a UAF on some bgfx
    // backends — mirror ShadowMapResources::destroy guard).
    if (BGFXAdapter::isValid(_lightingFbo)) {
        adapter.destroy(_lightingFbo);
        _lightingFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (BGFXAdapter::isValid(_fullscreenVB)) {
        adapter.destroy(_fullscreenVB);
        _fullscreenVB = bgfx::VertexBufferHandle{BGFX_INVALID_HANDLE};
    }
    if (BGFXAdapter::isValid(_fullscreenIB)) {
        adapter.destroy(_fullscreenIB);
        _fullscreenIB = bgfx::IndexBufferHandle{BGFX_INVALID_HANDLE};
    }
    _lightingW  = 0;
    _lightingH  = 0;
    _allocatedW = 0;
    _allocatedH = 0;
    _buildStamp = "";
    _program.reset();
    _programReady = false;
    _programAcquireFailed = false;
}

void LightingPass::ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    // §P5 B5 (2026-07-22) — mirror GBufferPass::ensure at
    // GBufferPass.cpp:268-314. Stamp-changed fast path + same-size
    // fast path; rebuild path on size/stamp change.
    if (!adapter.isInitialized() || width == 0 || height == 0) {
        return;
    }

    const bool stampChanged = (_buildStamp != kLightingBuildStamp);
    if (stampChanged) {
        _buildStamp = kLightingBuildStamp;
    }

    if (bgfx::isValid(_lightingFbo)
        && _allocatedW == width
        && _allocatedH == height
        && !stampChanged) {
        return;  // FBO cached
    }

    // Rebuild path — drop old FBO + recreate
    if (bgfx::isValid(_lightingFbo)) {
        adapter.destroy(_lightingFbo);
        _lightingFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _allocatedW = _allocatedH = 0;
    }

    // §P5 B5 (2026-07-22) — 1× RGBA8 LightingOutput FBO. NO depth
    // attachment — this is a fullscreen post-process pass that
    // does not read depth. `withDepth=false` matches cutsheet
    // `pass-lessons-from-deferred.md:151,161,169` "LightingPass
    // 写 LightingOutput FBO" semantics (independent RGBA8 RT, not
    // a color+depth like sceneFbo).
    _lightingFbo = adapter.createFrameBuffer(width, height,
                                              bgfx::TextureFormat::RGBA8,
                                              /*withDepth=*/false);
    if (bgfx::isValid(_lightingFbo)) {
        _allocatedW = width;
        _allocatedH = height;
        _lightingW = width;
        _lightingH = height;
    }
}

void LightingPass::ensureFullscreenQuad(BGFXAdapter& adapter)
{
    // §P5 B5 (2026-07-22) — mirror PostProcessPass::ensureFullscreenQuad
    // at PostProcessPass.cpp:280-309. Lazy creation; VB/IB cached
    // after first call.
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    _fullscreenVB = adapter.createVertexBuffer(kLightingFullscreenTriangle,
                                                sizeof(kLightingFullscreenTriangle),
                                                layout,
                                                BGFX_BUFFER_NONE);
    _fullscreenIB = adapter.createIndexBuffer(kLightingFullscreenIndices,
                                              sizeof(kLightingFullscreenIndices),
                                              BGFX_BUFFER_NONE);
}

void LightingPass::ensureProgram(ayt::shader::ShaderResourcePool& pool)
{
    // §P5 B5 (2026-07-22) — mirror GBufferPass::ensureProgram at
    // GBufferPass.cpp:72-107. Stamp-checked `s_acquiredCacheKey`
    // pointer-equal guard (ShadowCaster Issue 1 fix at
    // ShadowCaster.cpp:60-65). On compile failure: log + set
    // _programAcquireFailed, leave _programReady false. On success:
    // _program = acquired, _programReady = true.
    static const char* s_acquiredCacheKey = nullptr;
    if (s_acquiredCacheKey != kLightingCacheKey) {
        _program.reset();
        _programReady = false;
        _programAcquireFailed = false;
        s_acquiredCacheKey = kLightingCacheKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }

    ayt::shader::ShaderResource acquired =
        pool.acquire(kLightingPhoskiaSource, kLightingCacheKey);
    if (!acquired.isValid()) {
        _programAcquireFailed = true;
        std::fprintf(stderr,
                     "[LightingPass] acquire failed; LightingPass will "
                     "run as no-op (no GPU draw). Errors:\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[LightingPass]   %s\n", err.c_str());
        }
        return;
    }

    std::fprintf(stderr,
                 "[LightingPass] program ready via Phoskia (cacheKey=%s)\n",
                 kLightingCacheKey);

    _program = acquired;
    _programReady = true;
}

uint32_t LightingPass::execute(PassExecContext& ctx)
{
    // §P5 B5 (2026-07-22) — first real GPU work in the Deferred
    // LightingPass. Phoskia VS/FS acquires, binds view 8 to the
    // LightingOutput FBO, dispatches a fullscreen triangle that
    // samples 3 GBuffer attachments + applies Lambert directional
    // light (1 direction from FrameContext::lightDirection, 1 color
    // from FrameContext::lightColor).
    //
    // B5.5 (2026-07-22) — consumes `ctx.shadowPass->shadowMap()` via
    // the shared tryBindShadowSampler helper (mirror FO / Trans
    // call sites at ForwardOpaquePass.cpp:105-106 + TransparentPass:
    // 170-171). helper uploads `u_lightViewProj` +
    // `shadowBias` + `shadowMapTexel` + `shadowPcf` uniforms + binds
    // the shadowMap sampler on stage 1 (ShadowReceiverContract
    // kShadowMapName / kLightViewProjName / kShadowBiasName /
    // kShadowMapTexelName / kShadowPcfName). When ctx.shadowPass
    // is null or hasSampleableShadow()==false, helper falls back
    // to a fully-lit white texture + identity LVP so the FS
    // reads stay valid without UB.
    //
    // B7+ TAA boundary (unchanged): NOT consuming gbufferMotion
    // (binding present for TAA consumer symmetry, FS does not
    // read it — see cutsheet B5 boundary doc).
    //
    // §5.4 (2026-07-22, FO/Trans PR) — `isInitialized()` guard only,
    // NOT `|| isNoopBackend()`. Noop short-circuits inside
    // BGFXAdapter (each draw command is gated there); Pass-level
    // Noop gate would skip the scene-items loop on FO/Trans but
    // here the loop body is just 1 fullscreen-triangle submit so
    // the practical impact is small. Symmetry with FO/Trans fix
    // keeps the test semantic consistent: "logical draw count is
    // 1 on Noop".
    if (!ctx.adapter.isInitialized()) {
        return 0;
    }

    // Disable signal: host called setOutputSize(0, 0) (or never
    // called it). Mirror GBufferPass.cpp:134-136 size==0 early-out.
    if (_lightingW == 0 || _lightingH == 0) {
        return 0;
    }

    ensure(ctx.adapter, _lightingW, _lightingH);
    if (!bgfx::isValid(_lightingFbo)) {
        return 0;
    }

    ensureFullscreenQuad(ctx.adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    ensureProgram(ctx.pool);
    if (!_program.isValid()) {
        return 0;
    }

    const FrameContext& frame = ctx.frame;

    // View 8 wiring: bind LightingOutput FBO, set rect to viewport
    // size, clear, dispatch fullscreen triangle. Mirror GBufferPass
    // view 7 wiring at GBufferPass.cpp:157-176 but with
    // LightingOutput FBO + view 8.
    const uint8_t viewId = kLightingViewId;
    ctx.adapter.setViewTransform(viewId, frame.view, frame.projection);
    ctx.adapter.setViewFrameBuffer(viewId, _lightingFbo);
    ctx.adapter.setViewRect(viewId, 0, 0, _lightingW, _lightingH);
    // Clear to black (matches cutsheet §5.2 "GBuffer/Lighting 默认
    // clear=0" lock — black is the correct "no light contribution"
    // baseline; lit fragments overwrite).
    ctx.adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR,
                                /*rgba=*/0x000000ff,
                                /*depth=*/1.0f,
                                /*stencil=*/0);

    // B5 lighting state: WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS
    // (no depth read, no depth write — fullscreen post-process).
    // BGFXAdapter doesn't expose a setStatePostProcess helper yet,
    // so use the inline form via setStateOpaque() + override: we
    // bypass setStateOpaque and use bgfx::setState directly via
    // adapter.setStateOpaque() as the base (DEPTH_TEST_LESS +
    // CULL_CW) — but for a fullscreen post-process pass CULL_CW is
    // wrong (the oversize triangle needs no culling). Use
    // setStateDepthTestAlways() (verified at BGFXAdapter.cpp:171-178
    // documentation: "PostProcessPass fullscreen blit"). That's
    // WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS + no CULL_CW.
    ctx.adapter.setStateDepthTestAlways();

    // §P5 B5 (2026-07-22) — BIND 3 GBuffer samplers via borrowed
    // pointer to ctx.gbufferPass. The handles come from the
    // producer (B4a GBufferPass cacheAttachments) — we just read
    // them. Falls through cleanly if any is invalid (cutsheet
    // §1.7 "no work" signal — but B5 draws 1 submit regardless;
    // missing samplers surface as visual artifacts, not crashes).
    if (ctx.gbufferPass != nullptr) {
        // albedo sampler — stage from compiled Phoskia
        const shader::BindingId albedoBinding =
            _program.getTextureBinding("gbufferAlbedo");
        if (albedoBinding != shader::InvalidBinding) {
            bgfx::TextureHandle albedoHandle = ctx.gbufferPass->gbufferAlbedoRt();
            if (bgfx::isValid(albedoHandle)) {
                const uint8_t stage = _program.getTextureStage(albedoBinding);
                _program.setTexture(stage, albedoBinding,
                                    toShaderTexture(albedoHandle));
            }
        }
        // normal sampler
        const shader::BindingId normalBinding =
            _program.getTextureBinding("gbufferNormal");
        if (normalBinding != shader::InvalidBinding) {
            bgfx::TextureHandle normalHandle = ctx.gbufferPass->gbufferNormalRt();
            if (bgfx::isValid(normalHandle)) {
                const uint8_t stage = _program.getTextureStage(normalBinding);
                _program.setTexture(stage, normalBinding,
                                    toShaderTexture(normalHandle));
            }
        }
        // motion sampler (binding present, FS does not currently read it;
        // bound for B7+ TAA consumer symmetry — cutsheet B5 boundary)
        const shader::BindingId motionBinding =
            _program.getTextureBinding("gbufferMotion");
        if (motionBinding != shader::InvalidBinding) {
            bgfx::TextureHandle motionHandle = ctx.gbufferPass->gbufferMotionRt();
            if (bgfx::isValid(motionHandle)) {
                const uint8_t stage = _program.getTextureStage(motionBinding);
                _program.setTexture(stage, motionBinding,
                                    toShaderTexture(motionHandle));
            }
        }
    }

    // §P5 B5 (2026-07-22) — upload 3 light uniforms from FrameContext
    // (cutsheet §5.3 NO new FrameContext fields — reuse
    // lightDirection/lightColor/cameraPosition already shipped).
    //
    // lightDirection: FrameContext stores FROM-light; Phoskia NdotL
    // expects TO-light (same as ForwardOpaquePass / TransparentPass
    // `-frame.lightDirection`). Negate once on upload.
    //
    // Phoskia uniform ABI: vec4 uniforms are uploaded as vec4 (one
    // vec4 = bgfx Vec4 slot — see docs/pass-lessons-from-shadow.md
    // §3.1). 3-float vec3 → 4-float padded with .w = 0 (or 1 for
    // position-like).
    const shader::BindingId lightDirBinding =
        _program.getUniformBinding("u_lightDirection");
    if (lightDirBinding != shader::InvalidBinding) {
        // Match ForwardOpaquePass / TransparentPass: FrameContext stores
        // FROM-light; Phoskia NdotL expects TO-light → negate once.
        const float lightDir[4] = {
            -frame.lightDirection.x,
            -frame.lightDirection.y,
            -frame.lightDirection.z,
            0.0f,
        };
        _program.setUniform(lightDirBinding, lightDir, sizeof(lightDir));
    }

    const shader::BindingId lightColorBinding =
        _program.getUniformBinding("u_lightColor");
    if (lightColorBinding != shader::InvalidBinding) {
        const float lightColor[4] = {
            frame.lightColor.x,
            frame.lightColor.y,
            frame.lightColor.z,
            0.0f,
        };
        _program.setUniform(lightColorBinding, lightColor, sizeof(lightColor));
    }

    const shader::BindingId cameraPosBinding =
        _program.getUniformBinding("u_cameraPos");
    if (cameraPosBinding != shader::InvalidBinding) {
        const float cameraPos[4] = {
            frame.cameraPosition.x,
            frame.cameraPosition.y,
            frame.cameraPosition.z,
            1.0f,  // position-like → .w = 1
        };
        _program.setUniform(cameraPosBinding, cameraPos, sizeof(cameraPos));
    }

    // §P5 B7+ (2026-07-22) — multi-light DataSource upload via the
    // host-supplied `ctx.sceneLights` borrowed pointer. The Phoskia
    // FS unrolls 8 directional-light taps unconditionally. We pack
    // dir + color into the `uniformblock Lights { vec4 dirs[8];
    // vec4 colors[8]; } binding 0` UBO every frame.
    //
    // Mirrors Test_ShaderResource.cpp:392 (Camera UBO upload via
    // `setUniformBlock`) — one std140-compatible buffer, one
    // binding, no per-light state churn on the CPU.
    //
    // Layout (kMaxSceneLights = 8, defined in include/AYRenderScene.h):
    //   - bytes [0,    128): dirs[8] * vec4 = 8 * 16 = 128 bytes
    //   - bytes [128, 256): colors[8] * vec4 = 8 * 16 = 128 bytes
    //                     ↳ note we always upload the full
    //                       std140-aligned block regardless of
    //                       lights.count (Phoskia unroll uses
    //                       unused slots as zero-vectors, so a
    //                       count=2 light source still benefits
    //                       from the same 256-byte layout).
    //
    // Negate TO-light convention is identical to the B5 single-
    // light uniform above (FrameContext stores FROM-light; Phoskia
    // NdotL expects TO-light).
    //
    // Fallback: ctx.sceneLights == nullptr or count == 0 → upload
    // one "lights.dirs[0] = -frame.lightDirection" + "colors[0] =
    // frame.lightColor" pair so the unrolled FS still produces a
    // reasonable image (matches B5 behavior). This avoids the
    // "FS unrolls 8 lights but UBO has all zero → black picture"
    // failure mode when the host hasn't called setSceneLights yet.
    static_assert(ayt::render::kMaxSceneLights == 8,
                  "LightingPass B7 UBO assumes kMaxSceneLights == 8");

    alignas(16) float lightsBlock[ayt::render::kMaxSceneLights * 8] = {};  // 8 vec4 dirs + 8 vec4 colors
    const ayt::render::SceneLights* sceneLights = ctx.sceneLights;
    const bool useMulti = (sceneLights != nullptr) && (sceneLights->count > 0u);

    if (useMulti) {
        const uint32_t n = sceneLights->count;
        for (uint32_t i = 0; i < n && i < ayt::render::kMaxSceneLights; ++i) {
            const ayt::render::DirectionalLight& L = sceneLights->lights[i];
            // dirs[i] (16-byte vec4) — negate FrameContext convention
            lightsBlock[i * 4 + 0] = -L.direction.x;
            lightsBlock[i * 4 + 1] = -L.direction.y;
            lightsBlock[i * 4 + 2] = -L.direction.z;
            lightsBlock[i * 4 + 3] = 0.0f;
            // colors[i] (16-byte vec4)
            lightsBlock[(ayt::render::kMaxSceneLights + i) * 4 + 0] = L.color.x;
            lightsBlock[(ayt::render::kMaxSceneLights + i) * 4 + 1] = L.color.y;
            lightsBlock[(ayt::render::kMaxSceneLights + i) * 4 + 2] = L.color.z;
            lightsBlock[(ayt::render::kMaxSceneLights + i) * 4 + 3] = 0.0f;
        }
    } else {
        // B5 single-light fallback mirrors the FrameContext values
        // into lights[0]. The remaining 7 slots stay zero.
        lightsBlock[0] = -frame.lightDirection.x;
        lightsBlock[1] = -frame.lightDirection.y;
        lightsBlock[2] = -frame.lightDirection.z;
        lightsBlock[3] = 0.0f;
        lightsBlock[ayt::render::kMaxSceneLights * 4 + 0] = frame.lightColor.x;
        lightsBlock[ayt::render::kMaxSceneLights * 4 + 1] = frame.lightColor.y;
        lightsBlock[ayt::render::kMaxSceneLights * 4 + 2] = frame.lightColor.z;
        lightsBlock[ayt::render::kMaxSceneLights * 4 + 3] = 0.0f;
    }

    const shader::BindingId dirsBinding = _program.getUniformBinding("dirs");
    const shader::BindingId colorsBinding = _program.getUniformBinding("colors");
    if (dirsBinding != shader::InvalidBinding
        && colorsBinding != shader::InvalidBinding) {
        // HLSL field-split path: `uniform vec4 dirs[8]` + `colors[8]`.
        static bool s_loggedSplit = false;
        if (!s_loggedSplit) {
            s_loggedSplit = true;
            std::fprintf(stderr,
                         "[LightingPass] lights via field uniforms "
                         "dirs+colors (HLSL/bgfx)\n");
        }
        _program.setUniform(dirsBinding, lightsBlock,
                            ayt::render::kMaxSceneLights * 4u * sizeof(float));
        _program.setUniform(colorsBinding,
                            lightsBlock + ayt::render::kMaxSceneLights * 4u,
                            ayt::render::kMaxSceneLights * 4u * sizeof(float));
    } else {
        const shader::BindingId lightsUboBinding =
            _program.getUniformBlockBinding("Lights");
        if (lightsUboBinding != shader::InvalidBinding) {
            _program.setUniformBlock(lightsUboBinding,
                                     lightsBlock,
                                     sizeof(lightsBlock));
        } else {
            static bool s_loggedMissing = false;
            if (!s_loggedMissing) {
                s_loggedMissing = true;
                std::fprintf(stderr,
                             "[LightingPass] WARNING: no dirs/colors uniforms "
                             "and no Lights UBO — directional lighting skipped\n");
            }
        }
    }

    // §P5 B5.5 (2026-07-22) — consume ctx.shadowPass->shadowMap
    // via the shared `tryBindShadowSampler` helper (mirror the
    // FO / Trans call sites at ForwardOpaquePass.cpp:105-106 +
    // TransparentPass.cpp:170-171). The helper uploads 4 shadow
    // uniforms (`u_lightViewProj`, `shadowBias`, `shadowMapTexel`,
    // `shadowPcf`) and binds the shadowMap sampler on stage 1
    // (per ShadowReceiverContract::kShadowSamplerStage).
    //
    // Note on `flags`: LightingPass is a fullscreen post-process
    // (not a per-DrawItem receiver) so its shadow attenuation is
    // key-light only and we don't branch on per-mesh flags. The
    // helper's internal `wantSample = shouldSampleShadowMap(flags)`
    // short-circuits to `false` only when `ctx.shadowPass ==
    // nullptr` or `!hasSampleableShadow()` — both equal `false`
    // here. So the helper always uploads uniforms + binds the
    // sampler on stage 1 (or, on the Noop path, falls back to the
    // adapter's lit-white texture + identity LVP so the FS reads
    // still sample valid data without UB).
    //
    // Same shared-shadow contract for all 8 lights (cutsheet §10
    // pass-lessons-from-deferred.md:300 — "LightingPass 共用
    // ShadowPass 只读借用"): only lights[0] gets the shadow
    // attenuation. Fill / rim lights (1..7) stay unshadowed —
    // they don't have their own shadow map in v0. Per-light CSM
    // is a separate future cut.
    tryBindShadowSampler(_program, ctx.adapter, ctx.shadowPass,
                         kShadowCastAndReceive, frame.shadowBias);

    // §P5 B5.5 (2026-07-22) — worldPos reconstruction uniforms
    // (`u_invView`, `u_invProjection`). Computed per-frame as
    // inverses of frame.view / frame.projection in CPU (no new
    // FrameContext fields per cutsheet §5.3 — inverse lives on
    // the pass only). When `frame.view` / `frame.projection`
    // remain identity (test path, Noop un-init adapter), the
    // inverse is also identity, so worldPos = (ndc01.x, ndc01.y,
    // texDepth) — coarse but well-defined (the FS still has a
    // valid reconstruction; the key-light shadow attenuation
    // boils down to the Noop fallback fully-lit texture, which
    // yields shadowKey ≈ 1.0).
    {
        const ayt::math::Float4x4 invView = frame.view.inverse();
        const ayt::math::Float4x4 invProj = frame.projection.inverse();
        float colMajor[16];
        ayt::render::detail::toBgfxColumnMajor(invView, colMajor);
        const shader::BindingId invViewBinding =
            _program.getUniformBinding("u_invView");
        if (invViewBinding != shader::InvalidBinding) {
            _program.setUniform(invViewBinding, colMajor, sizeof(colMajor));
        }
        ayt::render::detail::toBgfxColumnMajor(invProj, colMajor);
        const shader::BindingId invProjBinding =
            _program.getUniformBinding("u_invProjection");
        if (invProjBinding != shader::InvalidBinding) {
            _program.setUniform(invProjBinding, colMajor, sizeof(colMajor));
        }
    }

    // §P5 B5.5 (2026-07-22) — bind the gbufferDepth D24S8 sampler
    // so the FS can reconstruct worldPos. Mirror FO / Trans
    // sampler-bind pattern at LightingPass.cpp:425-451 above.
    // When the GBufferPass has been ensured (B4a ship, runtime),
    // `gbufferDepthRt()` returns the cached D24S8 attachment
    // handle. When it hasn't, the sampler is unmapped and the
    // helper / Noop path falls through (the shadow reconstruction
    // is well-defined even without depth, but the bias term
    // becomes huge so we'd see "everything in shadow"); the
    // deferred-path guarantee is that GBufferPass::execute()
    // runs before LightingPass::execute() in `executeAll()`,
    // so on a real backend this is always populated.
    if (ctx.gbufferPass != nullptr) {
        const bgfx::TextureHandle depthHandle = ctx.gbufferPass->gbufferDepthRt();
        if (bgfx::isValid(depthHandle)) {
            const shader::BindingId depthBinding =
                _program.getTextureBinding("gbufferDepth");
            if (depthBinding != shader::InvalidBinding) {
                const uint8_t stage = _program.getTextureStage(depthBinding);
                _program.setTexture(stage, depthBinding,
                                    toShaderTexture(depthHandle));
            }
        }
    }

    // §P5 B5 (2026-07-22) — fullscreen triangle dispatch. B5
    // submits exactly 1 draw (the fullscreen triangle), not N.
    // `_program.submit()` writes viewId + draws using the
    // Adapter-set state. setTransform is a no-op for a fullscreen
    // post-process pass (no model matrix), but the API still wants
    // a valid identity; cheaper to skip — the FS only reads the
    // viewId and the state.
    ctx.adapter.setTransform(ayt::math::Float4x4::identity());
    ctx.adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    ctx.adapter.setIndexBuffer(_fullscreenIB, 0, 3);

    shader::DrawCallContext submitCtx;
    submitCtx.viewId = viewId;
    submitCtx.state  = 0;  // state owned by Adapter
    _program.submit(submitCtx);

    return 1;  // B5 ships exactly 1 draw (fullscreen triangle)
}

} // namespace ayt::render::detail