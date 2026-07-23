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
//   + `texture2d gbufferMotion` carries encoded worldPos (RT2;
//     GBufferFill v6) — Lighting decodes instead of sampling D24S8
//   + `texture2d shadowMap` + 4 shadow uniforms (u_lightViewProj,
//     shadowBias, shadowMapTexel, shadowPcf)
//   + FS key-light only PCF (9 taps) — fill / rim lights
//     (lights[1..7]) stay unshadowed.
//
// Single shared shadow map for the key light (cutsheet §10
// pass-lessons-from-deferred.md:300 — "LightingPass 共用
// ShadowPass 只读借用"). Per-light shadow maps is a separate
// future cut.
// §P5 B5.5 (2026-07-22) — v16: worldPos from GBuffer RT2 as
// RGBA16F raw xyz (no 8-bit encode). v15 RGBA8 encode caused
// ~0.16m quantization → mosaic shadow blocks on the ground.
// §P5.5 A (2026-07-23) — v19: keep UBO field name `dirs` (not `record`).
 // `record` as a bgfx/HLSL uniform identifier was a bad rename — host
 // upload / reflection mismatch left light vectors uninitialized
 // (flickering cyan fill, unstable key, missing shadows) while
 // `colors[]` still updated. `.w` still carries float(LightType).
 // §P5.5 B (2026-07-23) — bump to v21: Light POD widened with
 // Point/Spot per-light params (range/intensity/coneCosInner/
 // coneCosOuter) + spotDirection; Phoskia FS rewritten with
 // per-type attenuation + cone math (see fragment body below).
 // UBO widened to 4 vec4 arrays (dirs/colors/params/spotDir).
 // Bug fix #3 (Test_B5 cache-key false-green) is resolved by
 // `kLightingCacheKeyCStr` extern declared in LightingPass.h —
 // see definition below.
//
// §P5.5 D (2026-07-23) — bump v21 → v22: IBL MVP (Ambient Diffuse
// Cube Lookup). Phoskia source declares `texturecube envCube` +
// `uniform float cubeActive` (0/1 per-frame gate) +
// `uniform float ambientStrength` (default 0.6). The FS ambient
// term becomes `ambientFlat + ambientCube` where
//   ambientFlat = vec3(0.1, 0.1, 0.1)              // pre-D floor
//   ambientCube = sample(envCube, N).rgb * ambientStrength * cubeActive
// When cubeActive=0 (host never called Renderer::setSkySourceCube
// OR SkySource::kind != CubeMap) the cube branch contributes 0 —
// byte-equivalent to pre-D flat ambient. The single-FS +
// uniform-gating strategy mirrors §Skybox0 SkyboxPass dual-kind
// decision (avoid dual-FS program acquire overhead).
static constexpr const char* kLightingCacheKey =
    "lighting_v23_vec4_ibl_gates";

// §P5.5 B (2026-07-23) — Bug fix #3: single source of truth for
// cache-key string equality tests. The extern is declared in
// LightingPass.h; this is its definition.
const char* const kLightingCacheKeyCStr = kLightingCacheKey;

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
//
// §P5.5 B (2026-07-23) — Phoskia source rewritten for Point + Spot
// per-type math. UBO `Lights` widened from 2 vec4 arrays to 4
// (`dirs[8]` + `colors[8]` + `params[8]` + `spotDir[8]`). Each of
// the 8 light slots dispatches per-type inside an `if/else if`
// chain (Phoskia `fn` support not relied on — keeps the converter
// path simple). `params[]` carries (range, intensity, coneCosInner,
// coneCosOuter); `spotDir[].xyz` carries Spot direction (Point /
// Directional leave it zero — never read by their FS branches).
//
// Branch taxonomy (mirrors cutsheet §P5.5 B):
//   - Directional (w < 0.5):  NdotL * shadow (key only) * color
//   - Point (0.5 ≤ w < 1.5):  NdotL * (intensity / max(dist², 0.01))
//                              * (1 - smoothstep(0, range, dist))
//                              * color
//   - Spot (w ≥ 1.5):         same as Point PLUS
//                              smoothstep(coneCosOuter, coneCosInner, cosθ)
//                              where cosθ = dot(L_to_frag, spotDir)
//
// Shadow attenuation: ONLY lights[0] (key) gets shadowKey multiply,
// regardless of LightType. Fill / rim 1..7 stay unshadowed — mirror
// A's cutsheet §10 contract. If a host puts Point/Spot in slot 0,
// they get shadow attenuation for free (v0 simplification; per-light
// shadow is §P5.5 C scope).
constexpr const char* kLightingPhoskiaSource = R"(
uniformblock Lights {
    vec4 dirs[8]
    vec4 colors[8]
    vec4 params[8]
    vec4 spotDir[8]
} binding 0
material Lighting {
    texture2d gbufferAlbedo
    texture2d gbufferNormal
    texture2d gbufferMotion
    texture2d shadowMap
    texture2d gbufferSky
    texturecube envCube
    uniform vec4 u_lightDirection
    uniform vec4 u_lightColor
    uniform vec4 u_cameraPos
    uniform mat4 u_lightViewProj
    uniform vec4 shadowBias
    uniform vec4 shadowMapTexel
    uniform vec4 shadowPcf
    uniform vec4 skyMix
    uniform vec4 cubeActive
    uniform vec4 ambientStrength
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
        // §P5.5 D (2026-07-23) — IBL MVP ambient term. Default
        // cubeActive=0 ⇒ ambient = ambientFlat (pre-D byte-
        // equivalent). When host calls Renderer::setSkySourceCube
        // + SkySource::kind=CubeMap, cubeActive=1 and the cube
        // lookup adds normal-driven diffuse term scaled by
        // ambientStrength (default 0.6).
        let ambientFlat = vec3(0.1, 0.1, 0.1)
        let ambientCube = sample(envCube, N).rgb * ambientStrength.x * cubeActive.x
        let ambient = ambientFlat + ambientCube
        // §P5 B5.5 v16 — worldPos from GBuffer RT2 (RGBA16F raw xyz).
        let worldPos = sample(gbufferMotion, baseUv).xyz
        // §P5 B5.5 — Key-light shadow 9-tap PCF (cutsheet §10
        // "LightingPass 共用 ShadowPass 只读借用"). Fill / rim
        // lights (1..7) stay unshadowed — see per-type branches below.
        let clipPos = u_lightViewProj * vec4(worldPos, 1.0)
        let invW = 1.0 / max(clipPos.w, 0.0001)
        let ndcX = clipPos.x * invW
        let ndcY = clipPos.y * invW
        let refNdc01 = clipPos.z * invW * 0.5 + 0.5
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
        // §P5.5 B (2026-07-23) — per-light contribution. Each light
        // dispatches per-type via `dirs[i].w = float(LightType)`:
        //   - Directional: NdotL * shadow (key only) * color
        //   - Point:       NdotL * intensity / dist² * range-falloff * color
        //   - Spot:        Point path * cone (smoothstep coneCosOuter→inner) * color
        //
        // Empty slots have all-zero dirs[].xyz ⇒ toLight = (0,0,0) for
        // Point branch ⇒ dist = 0 ⇒ atten = 0 / 0.01 = 0 (safe — the
        // `max(..., 0.01)` divisor clamps). Directional branch reads
        // dirs[i].xyz directly; normalize(dirs[i].xyz) safely returns 0
        // because the `1.0 / max(length, 0.0001)` divisor clamps.
        //
        // Phoskia grammar note (2026-07-23): `if` is a *statement*, not
        // an expression — `let x = if (...) { ... } else { ... }` is
        // rejected by parsePrimary. Per-light contributions are
        // computed as 3 separate `let` values (dirContrib, pointContrib,
        // spotContrib), then selected via the GLSL `step(edge, x)`
        // builtin multiplied by the appropriate contribution. This is
        // a single multiply-add per light — same ALU cost as the
        // if-branched version on every backend, no assign-into-let
        // (which Phoskia doesn't support).
        // step(edge, x) returns 0 if x < edge else 1. So
        //   - isDir   = step(0.5, t0)  = 0 when t0 < 0.5  → Dir flag
        //                          (note: A uses LT-style; Phoskia
        //                          `step(edge, x)` is GE-style here
        //                          so we use 1.0 - step(...) for LT)
        //   - isPoint = step(0.5, t0) - step(1.5, t0)
        //   - isSpot  = step(1.5, t0)
        // Wait — verify: GLSL `step(edge, x) = (x < edge) ? 0 : 1`.
        // Actually: GLSL `step(genType edge, genType x)` returns
        // `0.0` if `x < edge`, else `1.0`. So:
        //   - t0 < 0.5:  step(0.5, t0) = 0
        //   - 0.5 ≤ t0 < 1.5: step(0.5,t0)=1, step(1.5,t0)=0
        //   - t0 ≥ 1.5: step(1.5, t0) = 1
        // So: isDir = 1.0 - step(0.5, t0)
        //     isPoint = step(0.5, t0) - step(1.5, t0)
        //     isSpot  = step(1.5, t0)
        // (sum to 1.0 exactly when t0 is one of 0.0/1.0/2.0, which is
        //  the only values `dirs[i].w = float(LightType)` ever takes.)
        //
        // §P5.5 B (2026-07-23) — shadowKey multiply stays on the
        // Directional key-light contribution only (lights[0]).
        // Fill/rim 1..7 stay unshadowed. To apply shadowKey to the
        // Dir branch but not the others, we compute:
        //   keyShadowMul = isDir  (so Point/Spot branches collapse
        //                          the shadowKey multiply to 0)
        // ... and apply shadowKey * keyShadowMul in the final mix.
        //
        // --- light 0 (key — gets shadowKey via isDir gate) ---
        let Ld0 = Lights.dirs[0].xyz * (1.0 / max(length(Lights.dirs[0].xyz), 0.0001))
        let toL0 = Lights.dirs[0].xyz - worldPos
        let d0 = length(toL0)
        let Lp0 = toL0 * (1.0 / max(d0, 0.0001))
        let sdn0 = Lights.spotDir[0].xyz * (1.0 / max(length(Lights.spotDir[0].xyz), 0.0001))
        let cosT0 = -1.0 * dot(Lp0, sdn0)
        let cone0 = smoothstep(Lights.params[0].w, Lights.params[0].z, cosT0)
        let falloff0 = 1.0 - smoothstep(0.0, Lights.params[0].x, d0)
        let attenPoint0 = Lights.params[0].y * falloff0 / max(d0 * d0, 0.01)
        let attenSpot0  = Lights.params[0].y * falloff0 * cone0 / max(d0 * d0, 0.01)
        let NdotDir0  = max(dot(N, Ld0), 0.0)
        let NdotPos0  = max(dot(N, Lp0), 0.0)
        let dirPart0  = NdotDir0 * shadowKey * Lights.colors[0].xyz
        let pointPart0 = NdotPos0 * attenPoint0 * Lights.colors[0].xyz
        let spotPart0  = NdotPos0 * attenSpot0  * Lights.colors[0].xyz
        let isDir0  = 1.0 - step(0.5, Lights.dirs[0].w)
        let isPoint0 = step(0.5, Lights.dirs[0].w) - step(1.5, Lights.dirs[0].w)
        let isSpot0 = step(1.5, Lights.dirs[0].w)
        let keyContrib = dirPart0 * isDir0 + pointPart0 * isPoint0 + spotPart0 * isSpot0
        // --- lights 1..7 (fill/rim — no shadow multiply) ---
        let Ld1 = Lights.dirs[1].xyz * (1.0 / max(length(Lights.dirs[1].xyz), 0.0001))
        let toL1 = Lights.dirs[1].xyz - worldPos
        let d1 = length(toL1)
        let Lp1 = toL1 * (1.0 / max(d1, 0.0001))
        let sdn1 = Lights.spotDir[1].xyz * (1.0 / max(length(Lights.spotDir[1].xyz), 0.0001))
        let cosT1 = -1.0 * dot(Lp1, sdn1)
        let cone1 = smoothstep(Lights.params[1].w, Lights.params[1].z, cosT1)
        let falloff1 = 1.0 - smoothstep(0.0, Lights.params[1].x, d1)
        let attenPoint1 = Lights.params[1].y * falloff1 / max(d1 * d1, 0.01)
        let attenSpot1  = Lights.params[1].y * falloff1 * cone1 / max(d1 * d1, 0.01)
        let NdotDir1 = max(dot(N, Ld1), 0.0)
        let NdotPos1 = max(dot(N, Lp1), 0.0)
        let dirPart1 = NdotDir1 * Lights.colors[1].xyz
        let pointPart1 = NdotPos1 * attenPoint1 * Lights.colors[1].xyz
        let spotPart1 = NdotPos1 * attenSpot1 * Lights.colors[1].xyz
        let isDir1 = 1.0 - step(0.5, Lights.dirs[1].w)
        let isPoint1 = step(0.5, Lights.dirs[1].w) - step(1.5, Lights.dirs[1].w)
        let isSpot1 = step(1.5, Lights.dirs[1].w)
        let f1 = dirPart1 * isDir1 + pointPart1 * isPoint1 + spotPart1 * isSpot1
        let Ld2 = Lights.dirs[2].xyz * (1.0 / max(length(Lights.dirs[2].xyz), 0.0001))
        let toL2 = Lights.dirs[2].xyz - worldPos
        let d2 = length(toL2)
        let Lp2 = toL2 * (1.0 / max(d2, 0.0001))
        let sdn2 = Lights.spotDir[2].xyz * (1.0 / max(length(Lights.spotDir[2].xyz), 0.0001))
        let cosT2 = -1.0 * dot(Lp2, sdn2)
        let cone2 = smoothstep(Lights.params[2].w, Lights.params[2].z, cosT2)
        let falloff2 = 1.0 - smoothstep(0.0, Lights.params[2].x, d2)
        let attenPoint2 = Lights.params[2].y * falloff2 / max(d2 * d2, 0.01)
        let attenSpot2  = Lights.params[2].y * falloff2 * cone2 / max(d2 * d2, 0.01)
        let NdotDir2 = max(dot(N, Ld2), 0.0)
        let NdotPos2 = max(dot(N, Lp2), 0.0)
        let dirPart2 = NdotDir2 * Lights.colors[2].xyz
        let pointPart2 = NdotPos2 * attenPoint2 * Lights.colors[2].xyz
        let spotPart2 = NdotPos2 * attenSpot2 * Lights.colors[2].xyz
        let isDir2 = 1.0 - step(0.5, Lights.dirs[2].w)
        let isPoint2 = step(0.5, Lights.dirs[2].w) - step(1.5, Lights.dirs[2].w)
        let isSpot2 = step(1.5, Lights.dirs[2].w)
        let f2 = dirPart2 * isDir2 + pointPart2 * isPoint2 + spotPart2 * isSpot2
        let Ld3 = Lights.dirs[3].xyz * (1.0 / max(length(Lights.dirs[3].xyz), 0.0001))
        let toL3 = Lights.dirs[3].xyz - worldPos
        let d3 = length(toL3)
        let Lp3 = toL3 * (1.0 / max(d3, 0.0001))
        let sdn3 = Lights.spotDir[3].xyz * (1.0 / max(length(Lights.spotDir[3].xyz), 0.0001))
        let cosT3 = -1.0 * dot(Lp3, sdn3)
        let cone3 = smoothstep(Lights.params[3].w, Lights.params[3].z, cosT3)
        let falloff3 = 1.0 - smoothstep(0.0, Lights.params[3].x, d3)
        let attenPoint3 = Lights.params[3].y * falloff3 / max(d3 * d3, 0.01)
        let attenSpot3  = Lights.params[3].y * falloff3 * cone3 / max(d3 * d3, 0.01)
        let NdotDir3 = max(dot(N, Ld3), 0.0)
        let NdotPos3 = max(dot(N, Lp3), 0.0)
        let dirPart3 = NdotDir3 * Lights.colors[3].xyz
        let pointPart3 = NdotPos3 * attenPoint3 * Lights.colors[3].xyz
        let spotPart3 = NdotPos3 * attenSpot3 * Lights.colors[3].xyz
        let isDir3 = 1.0 - step(0.5, Lights.dirs[3].w)
        let isPoint3 = step(0.5, Lights.dirs[3].w) - step(1.5, Lights.dirs[3].w)
        let isSpot3 = step(1.5, Lights.dirs[3].w)
        let f3 = dirPart3 * isDir3 + pointPart3 * isPoint3 + spotPart3 * isSpot3
        let Ld4 = Lights.dirs[4].xyz * (1.0 / max(length(Lights.dirs[4].xyz), 0.0001))
        let toL4 = Lights.dirs[4].xyz - worldPos
        let d4 = length(toL4)
        let Lp4 = toL4 * (1.0 / max(d4, 0.0001))
        let sdn4 = Lights.spotDir[4].xyz * (1.0 / max(length(Lights.spotDir[4].xyz), 0.0001))
        let cosT4 = -1.0 * dot(Lp4, sdn4)
        let cone4 = smoothstep(Lights.params[4].w, Lights.params[4].z, cosT4)
        let falloff4 = 1.0 - smoothstep(0.0, Lights.params[4].x, d4)
        let attenPoint4 = Lights.params[4].y * falloff4 / max(d4 * d4, 0.01)
        let attenSpot4  = Lights.params[4].y * falloff4 * cone4 / max(d4 * d4, 0.01)
        let NdotDir4 = max(dot(N, Ld4), 0.0)
        let NdotPos4 = max(dot(N, Lp4), 0.0)
        let dirPart4 = NdotDir4 * Lights.colors[4].xyz
        let pointPart4 = NdotPos4 * attenPoint4 * Lights.colors[4].xyz
        let spotPart4 = NdotPos4 * attenSpot4 * Lights.colors[4].xyz
        let isDir4 = 1.0 - step(0.5, Lights.dirs[4].w)
        let isPoint4 = step(0.5, Lights.dirs[4].w) - step(1.5, Lights.dirs[4].w)
        let isSpot4 = step(1.5, Lights.dirs[4].w)
        let f4 = dirPart4 * isDir4 + pointPart4 * isPoint4 + spotPart4 * isSpot4
        let Ld5 = Lights.dirs[5].xyz * (1.0 / max(length(Lights.dirs[5].xyz), 0.0001))
        let toL5 = Lights.dirs[5].xyz - worldPos
        let d5 = length(toL5)
        let Lp5 = toL5 * (1.0 / max(d5, 0.0001))
        let sdn5 = Lights.spotDir[5].xyz * (1.0 / max(length(Lights.spotDir[5].xyz), 0.0001))
        let cosT5 = -1.0 * dot(Lp5, sdn5)
        let cone5 = smoothstep(Lights.params[5].w, Lights.params[5].z, cosT5)
        let falloff5 = 1.0 - smoothstep(0.0, Lights.params[5].x, d5)
        let attenPoint5 = Lights.params[5].y * falloff5 / max(d5 * d5, 0.01)
        let attenSpot5  = Lights.params[5].y * falloff5 * cone5 / max(d5 * d5, 0.01)
        let NdotDir5 = max(dot(N, Ld5), 0.0)
        let NdotPos5 = max(dot(N, Lp5), 0.0)
        let dirPart5 = NdotDir5 * Lights.colors[5].xyz
        let pointPart5 = NdotPos5 * attenPoint5 * Lights.colors[5].xyz
        let spotPart5 = NdotPos5 * attenSpot5 * Lights.colors[5].xyz
        let isDir5 = 1.0 - step(0.5, Lights.dirs[5].w)
        let isPoint5 = step(0.5, Lights.dirs[5].w) - step(1.5, Lights.dirs[5].w)
        let isSpot5 = step(1.5, Lights.dirs[5].w)
        let f5 = dirPart5 * isDir5 + pointPart5 * isPoint5 + spotPart5 * isSpot5
        let Ld6 = Lights.dirs[6].xyz * (1.0 / max(length(Lights.dirs[6].xyz), 0.0001))
        let toL6 = Lights.dirs[6].xyz - worldPos
        let d6 = length(toL6)
        let Lp6 = toL6 * (1.0 / max(d6, 0.0001))
        let sdn6 = Lights.spotDir[6].xyz * (1.0 / max(length(Lights.spotDir[6].xyz), 0.0001))
        let cosT6 = -1.0 * dot(Lp6, sdn6)
        let cone6 = smoothstep(Lights.params[6].w, Lights.params[6].z, cosT6)
        let falloff6 = 1.0 - smoothstep(0.0, Lights.params[6].x, d6)
        let attenPoint6 = Lights.params[6].y * falloff6 / max(d6 * d6, 0.01)
        let attenSpot6  = Lights.params[6].y * falloff6 * cone6 / max(d6 * d6, 0.01)
        let NdotDir6 = max(dot(N, Ld6), 0.0)
        let NdotPos6 = max(dot(N, Lp6), 0.0)
        let dirPart6 = NdotDir6 * Lights.colors[6].xyz
        let pointPart6 = NdotPos6 * attenPoint6 * Lights.colors[6].xyz
        let spotPart6 = NdotPos6 * attenSpot6 * Lights.colors[6].xyz
        let isDir6 = 1.0 - step(0.5, Lights.dirs[6].w)
        let isPoint6 = step(0.5, Lights.dirs[6].w) - step(1.5, Lights.dirs[6].w)
        let isSpot6 = step(1.5, Lights.dirs[6].w)
        let f6 = dirPart6 * isDir6 + pointPart6 * isPoint6 + spotPart6 * isSpot6
        let Ld7 = Lights.dirs[7].xyz * (1.0 / max(length(Lights.dirs[7].xyz), 0.0001))
        let toL7 = Lights.dirs[7].xyz - worldPos
        let d7 = length(toL7)
        let Lp7 = toL7 * (1.0 / max(d7, 0.0001))
        let sdn7 = Lights.spotDir[7].xyz * (1.0 / max(length(Lights.spotDir[7].xyz), 0.0001))
        let cosT7 = -1.0 * dot(Lp7, sdn7)
        let cone7 = smoothstep(Lights.params[7].w, Lights.params[7].z, cosT7)
        let falloff7 = 1.0 - smoothstep(0.0, Lights.params[7].x, d7)
        let attenPoint7 = Lights.params[7].y * falloff7 / max(d7 * d7, 0.01)
        let attenSpot7  = Lights.params[7].y * falloff7 * cone7 / max(d7 * d7, 0.01)
        let NdotDir7 = max(dot(N, Ld7), 0.0)
        let NdotPos7 = max(dot(N, Lp7), 0.0)
        let dirPart7 = NdotDir7 * Lights.colors[7].xyz
        let pointPart7 = NdotPos7 * attenPoint7 * Lights.colors[7].xyz
        let spotPart7 = NdotPos7 * attenSpot7 * Lights.colors[7].xyz
        let isDir7 = 1.0 - step(0.5, Lights.dirs[7].w)
        let isPoint7 = step(0.5, Lights.dirs[7].w) - step(1.5, Lights.dirs[7].w)
        let isSpot7 = step(1.5, Lights.dirs[7].w)
        let f7 = dirPart7 * isDir7 + pointPart7 * isPoint7 + spotPart7 * isSpot7
        let directionalSum = keyContrib + f1 + f2 + f3 + f4 + f5 + f6 + f7
        let lit = albedo.rgb * (ambient + directionalSum)
        // §Skybox0 (2026-07-23) — backdrop blend: sky only shows
        // where lit is near zero (so geometry keeps its color;
        // sky fills gaps in scene coverage). When
        // `ctx.skyboxPass == nullptr` the gbufferSky sampler stays
        // unbound and `sample(gbufferSky, baseUv)` returns black;
        // `mix(black, lit, 1) == lit` collapses to the pre-§Skybox0
        // dark-frame behavior on a Forward / non-sky host.
        let skyColor = sample(gbufferSky, baseUv).xyz * skyMix.x
        let coverage = step(0.001, max(max(lit.r, lit.g), lit.b))
        let finalColor = mix(skyColor, lit, coverage)
        return vec4(finalColor, albedo.a)
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
    // §P5.5 D (2026-07-23) — cube binding IDs reset on destroy so
    // the next ensureProgram() re-resolves them after the v22
    // cache-key bump forces a re-acquire.
    _tEnvCube         = ayt::shader::InvalidBinding;
    _uCubeActive      = ayt::shader::InvalidBinding;
    _uAmbientStrength = ayt::shader::InvalidBinding;
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
        // §P5.5 D (2026-07-23) — cube binding IDs reset on cache-
        // key bump (v21 → v22). Mirror shadowMap / gbufferSky
        // binding reset pattern at the top of ensureProgram.
        _tEnvCube         = ayt::shader::InvalidBinding;
        _uCubeActive      = ayt::shader::InvalidBinding;
        _uAmbientStrength = ayt::shader::InvalidBinding;
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

    // §P5.5 D (2026-07-23) — lazy-resolve IBL MVP binding IDs.
    // Default InvalidBinding on acquire failure; the FS's
    // `ambientCube * cubeActive` collapses to 0 (ambientFlat only)
    // when cubeActive=0 OR envCube binding is invalid. The
    // cubeActive uniform binding is the per-frame gate that
    // distinguishes pre-D flat ambient vs normal-driven ambient.
    _tEnvCube         = _program.getTextureBinding("envCube");
    _uCubeActive      = _program.getUniformBinding("cubeActive");
    _uAmbientStrength = _program.getUniformBinding("ambientStrength");
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
        // §Skybox0 (2026-07-23) — gbufferSky backdrop sampler.
        // Mirror gbufferAlbedoRt / gbufferNormalRt / gbufferMotionRt
        // shape: lazy-resolve binding, bind from ctx.skyboxPass
        // (SkyboxPass-borrowed-pointer mirror), cache the handle in
        // `_gbufferSkyRt` for symmetry with the other GBuffer RTs.
        // When ctx.skyboxPass == nullptr (Forward path / no sky
        // mounted) the sampler stays unbound and
        // `sample(gbufferSky, baseUv)` returns black, collapsing
        // the backdrop blend to `lit` (pre-§Skybox0 behavior).
        const shader::BindingId skyBinding =
            _program.getTextureBinding("gbufferSky");
        if (skyBinding != shader::InvalidBinding
            && ctx.skyboxPass != nullptr) {
            bgfx::TextureHandle skyHandle = ctx.skyboxPass->skyRt();
            if (bgfx::isValid(skyHandle)) {
                const uint8_t stage = _program.getTextureStage(skyBinding);
                _program.setTexture(stage, skyBinding,
                                    toShaderTexture(skyHandle));
                _gbufferSkyRt = skyHandle;
            } else {
                _gbufferSkyRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
            }
        }
    }

    // §Skybox0 (2026-07-23) — upload `skyMix` uniform. Default =
    // 1.0 (full intensity). Host can override per-material via
    // `Renderer::setMaterialVec3(material, "skyMix", v)`. Phoskia
    // Vec4 ABI (bgfx Vec4 slot, see docs/pass-lessons-from-shadow.md
    // §3.1) — scalar in .x, pad .yzw = 0. Bound unconditionally —
    // when the gbufferSky sampler is unbound the skyMix value has
    // no observable effect (the `sample` returns black regardless),
    // so this upload is safe on both Forward and Deferred paths.
    const shader::BindingId skyMixBinding =
        _program.getUniformBinding("skyMix");
    if (skyMixBinding != shader::InvalidBinding) {
        const float skyMixPad[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        _program.setUniform(skyMixBinding, skyMixPad, sizeof(skyMixPad));
    }

    // §P5.5 D (2026-07-23) — IBL MVP cube path: bind envCube
    // sampler + upload cubeActive / ambientStrength uniforms.
    // cube handle comes from SkyboxPass producer state
    // (setSkySourceCube). IBL is independent of SkySource::kind:
    // equirect can remain the backdrop (skyKind=0) while envCube
    // still feeds ambientCube. cubeActive=0 only when no cube
    // handle / no SkyboxPass / texture missing from ctx.textures.
    bool cubeActive = false;
    if (ctx.skyboxPass != nullptr
        && ctx.skyboxPass->hasCubeTexture()) {
        const auto cubeIt = ctx.textures.find(
            ctx.skyboxPass->cubeTexture().id);
        if (cubeIt != ctx.textures.end()
            && BGFXAdapter::isValid(cubeIt->second.handle)) {
            const shader::BindingId envCubeBinding =
                _program.getTextureBinding("envCube");
            if (envCubeBinding != shader::InvalidBinding) {
                const uint8_t stage = _program.getTextureStage(envCubeBinding);
                _program.setTexture(stage, envCubeBinding,
                                    toShaderTexture(cubeIt->second.handle));
            }
            cubeActive = true;
        }
    }

    // §P5.5 D (2026-07-23) — upload cubeActive (0.0 or 1.0) +
    // ambientStrength (default 0.6). cubeActive mirrors the same
    // predicate SkyboxPass::execute uses for skyKind so the two
    // paths can never disagree per frame. ambientStrength is a
    // constant scalar — the host can override via per-material
    // setMaterialFloat(material, "ambientStrength", v) if a future
    // cut exposes it; D ships the default and pins the value
    // here.
    //
    // Upload-shape note: bgfx's `setUniform` writes a vec4 slot
    // regardless of the Phoskia-declared type (uniform float is
    // emitted as `uniform float foo;` by AYBGFXConverter but the
    // CPU upload path always takes 16 bytes; HLSL auto-pads
    // scalars into the cbuffer vec4 slot). All other uniforms in
    // this pass (skyMix / u_lightDirection / ...) follow the
    // same 16-byte padded upload pattern — see RenderPass.cpp
    // tryBindShadowSampler + LightingPass.cpp skyMix upload
    // above. Using sizeof(float)=4 would under-write the slot
    // and the HLSL `uniform float foo;` read would sample
    // garbage.
    const shader::BindingId cubeActiveBinding =
        _program.getUniformBinding("cubeActive");
    if (cubeActiveBinding != shader::InvalidBinding) {
        const float cubeActivePad[4] = {
            cubeActive ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f
        };
        _program.setUniform(cubeActiveBinding, cubeActivePad,
                            sizeof(cubeActivePad));
    }
    const shader::BindingId ambientStrengthBinding =
        _program.getUniformBinding("ambientStrength");
    if (ambientStrengthBinding != shader::InvalidBinding) {
        const float ambientStrengthPad[4] = { _ambientStrength, 0.0f, 0.0f, 0.0f };
        _program.setUniform(ambientStrengthBinding, ambientStrengthPad,
                            sizeof(ambientStrengthPad));
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

    // §P5 B7+ (2026-07-22) + §P5.5 A (2026-07-23) + §P5.5 B (2026-07-23)
    // — multi-light DataSource upload via the host-supplied
    // `ctx.sceneLights` borrowed pointer. The Phoskia FS unrolls 8
    // light taps with per-type dispatch (`dirs[i].w = LightType`).
    // We pack per-light GPU-side data into the
    // `uniformblock Lights { vec4 dirs[8]; vec4 colors[8]; vec4 params[8];
    // vec4 spotDir[8]; } binding 0` UBO every frame.
    //
    // B widens the UBO from 2 vec4 arrays (256B) to 4 vec4 arrays
    // (512B). The new arrays carry per-light attenuation / cone
    // params (`params[]`) and Spot direction (`spotDir[]`); the
    // `dirs[]` + `colors[]` array shapes stay byte-identical to A
    // (cutsheet §P5.5 B #3 invariant: default LightType = Directional
    // ⇒ byte-equivalent to pre-B).
    //
    // Per-type CPU pack (cutsheet §P5.5 B):
    //   - Directional : dirs[].xyz = -direction (FROM-light negated)
    //                   params[] = default (range=0, intensity=1,
    //                                       coneCosInner=0, coneCosOuter=0)
    //                   spotDir[] = default (0, -1, 0, 0)
    //   - Point       : dirs[].xyz = +position (FS does
    //                                 `worldPos - lightPos`)
    //                   params[] = (range, intensity, 0, 0)
    //                   spotDir[] = default (Point branch never reads)
    //   - Spot        : dirs[].xyz = +position (same as Point)
    //                   params[] = (range, intensity, coneCosInner, coneCosOuter)
    //                   spotDir[] = (spotDirection.xyz, 0)
    //
    // Mirrors Test_ShaderResource.cpp:392 (Camera UBO upload via
    // `setUniformBlock`) — one std140-compatible buffer, one binding.
    //
    // Layout (kMaxSceneLights = 8, defined in include/AYRenderScene.h):
    //   - bytes [0,   128): dirs[8] * vec4 = 8 * 16 = 128 bytes
    //                       (xyz = light vector, w = float(LightType))
    //   - bytes [128, 256): colors[8] * vec4 = 8 * 16 = 128 bytes
    //   - bytes [256, 384): params[8] * vec4 = 8 * 16 = 128 bytes (B new)
    //                       (x = range, y = intensity, z = coneCosInner,
    //                        w = coneCosOuter)
    //   - bytes [384, 512): spotDir[8] * vec4 = 8 * 16 = 128 bytes (B new)
    //                       (xyz = spotDirection, w = 0)
    // Always upload the full 512-byte layout regardless of
    // lights.count — Phoskia unroll uses unused slots as zero-vectors.
    //
    // Fallback: ctx.sceneLights == nullptr or count == 0 → upload
    // one "dirs[0] = -frame.lightDirection, colors[0] = frame.lightColor"
    // pair so the unrolled FS still produces a reasonable image
    // (matches B5 single-light behavior). Params + spotDir slots stay
    // zero — Directional branch never reads them. This avoids the
    // "FS unrolls 8 lights but UBO has all zero → black picture"
    // failure mode when the host hasn't called setSceneLights yet.
    static_assert(ayt::render::kMaxSceneLights == 8,
                  "LightingPass B7 UBO assumes kMaxSceneLights == 8");

    alignas(16) float lightsBlock[ayt::render::kMaxSceneLights * 16] = {};
    // 8 vec4 dirs + 8 vec4 colors + 8 vec4 params + 8 vec4 spotDir
    // = 32 floats × 4 bytes = 512 bytes total
    const ayt::render::SceneLights* sceneLights = ctx.sceneLights;
    const bool useMulti = (sceneLights != nullptr) && (sceneLights->count > 0u);

    if (useMulti) {
        const uint32_t n = sceneLights->count;
        for (uint32_t i = 0; i < n && i < ayt::render::kMaxSceneLights; ++i) {
            const ayt::render::Light& L = sceneLights->lights[i];
            // dirs[i] (16-byte vec4) — per LightType CPU-side split.
            // xyz + .w = float(LightType). Point/Spot branches land
            // at zero CPU cost (the switch is a jump table).
            float lightVecX = 0.0f, lightVecY = 0.0f, lightVecZ = 0.0f;
            float paramRange = 0.0f, paramIntensity = 1.0f;
            float paramConeInner = 0.0f, paramConeOuter = 0.0f;
            float spotDirX = 0.0f, spotDirY = -1.0f, spotDirZ = 0.0f;
            switch (L.type) {
            case ayt::render::LightType::Directional:
                // Negate FrameContext convention.
                lightVecX = -L.direction.x;
                lightVecY = -L.direction.y;
                lightVecZ = -L.direction.z;
                // params[] + spotDir[] stay at default — Directional
                // branch never reads them.
                break;
            case ayt::render::LightType::Point:
                lightVecX = L.position.x;
                lightVecY = L.position.y;
                lightVecZ = L.position.z;
                paramRange    = L.range;
                paramIntensity = L.intensity;
                // spotDir[] stays at default — Point branch never reads it.
                break;
            case ayt::render::LightType::Spot:
                lightVecX = L.position.x;
                lightVecY = L.position.y;
                lightVecZ = L.position.z;
                paramRange     = L.range;
                paramIntensity = L.intensity;
                paramConeInner = L.coneCosInner;
                paramConeOuter = L.coneCosOuter;
                spotDirX = L.spotDirection.x;
                spotDirY = L.spotDirection.y;
                spotDirZ = L.spotDirection.z;
                break;
            }
            // dirs[i] (16-byte vec4) — xyz + float(LightType).
            lightsBlock[i * 4 + 0] = lightVecX;
            lightsBlock[i * 4 + 1] = lightVecY;
            lightsBlock[i * 4 + 2] = lightVecZ;
            lightsBlock[i * 4 + 3] =
                static_cast<float>(L.type);
            // colors[i] (16-byte vec4) — same for all light types.
            lightsBlock[(ayt::render::kMaxSceneLights + i) * 4 + 0] = L.color.x;
            lightsBlock[(ayt::render::kMaxSceneLights + i) * 4 + 1] = L.color.y;
            lightsBlock[(ayt::render::kMaxSceneLights + i) * 4 + 2] = L.color.z;
            lightsBlock[(ayt::render::kMaxSceneLights + i) * 4 + 3] = 0.0f;
            // params[i] (16-byte vec4) — B: per-light (range, intensity,
            // coneCosInner, coneCosOuter). Directional leaves them at
            // (0, 1, 0, 0) — the FS Directional branch never reads this
            // array, but the upload is unconditional so any stray FS
            // code that does (e.g. debug viz) sees the right values.
            lightsBlock[(ayt::render::kMaxSceneLights * 2 + i) * 4 + 0] = paramRange;
            lightsBlock[(ayt::render::kMaxSceneLights * 2 + i) * 4 + 1] = paramIntensity;
            lightsBlock[(ayt::render::kMaxSceneLights * 2 + i) * 4 + 2] = paramConeInner;
            lightsBlock[(ayt::render::kMaxSceneLights * 2 + i) * 4 + 3] = paramConeOuter;
            // spotDir[i] (16-byte vec4) — B: Spot direction (xyz) + 0
            // padding. Directional/Point leave (0, -1, 0, 0) default
            // (their FS branches never read this array).
            lightsBlock[(ayt::render::kMaxSceneLights * 3 + i) * 4 + 0] = spotDirX;
            lightsBlock[(ayt::render::kMaxSceneLights * 3 + i) * 4 + 1] = spotDirY;
            lightsBlock[(ayt::render::kMaxSceneLights * 3 + i) * 4 + 2] = spotDirZ;
            lightsBlock[(ayt::render::kMaxSceneLights * 3 + i) * 4 + 3] = 0.0f;
        }
    } else {
        // B5 single-light fallback mirrors the FrameContext values
        // into dirs[0] + colors[0]. The remaining 7 slots stay zero.
        // B widens the layout to 4 arrays but the fallback only writes
        // the first 2 (params + spotDir stay zero — Directional branch
        // never reads them, and the legacy single-light contract is
        // preserved byte-for-byte at the first 32 bytes of lightsBlock).
        lightsBlock[0] = -frame.lightDirection.x;
        lightsBlock[1] = -frame.lightDirection.y;
        lightsBlock[2] = -frame.lightDirection.z;
        lightsBlock[3] = 0.0f;  // w = float(LightType::Directional) = 0
        lightsBlock[ayt::render::kMaxSceneLights * 4 + 0] = frame.lightColor.x;
        lightsBlock[ayt::render::kMaxSceneLights * 4 + 1] = frame.lightColor.y;
        lightsBlock[ayt::render::kMaxSceneLights * 4 + 2] = frame.lightColor.z;
        lightsBlock[ayt::render::kMaxSceneLights * 4 + 3] = 0.0f;
    }

    // §P5.5 B (2026-07-23) — force single field-split upload path,
    // REMOVE the setUniformBlock fallback. Reasoning (cutsheet §P5.5 B
    // dual-path decision):
    //   - A's dual-path was a workaround when the UBO field rename
    //     `dirs→record` broke HLSL/bgfx uniform wiring. Field-split
    //     (`setUniform` per vec4 array) is the path that's actually
    //     stable today.
    //   - B adds 2 more vec4 arrays (`params[8]` + `spotDir[8]`); a
    //     dual-path on 4 arrays would explode to 2^4 = 16 combinations.
    //   - When to migrate to setUniformBlock: post-B ship, on D3D
    //     3-run validate, then a future cut can collapse.
    //
    // 4 setUniform calls (one per array), all 128 bytes each.
    // Logged-once message confirms we're on the B field-split path.
    // §P5.5 A — field name stays `dirs` (xyz = light vector, w = type).
    // Do NOT rename to `record` — that broke HLSL/bgfx uniform wiring.
    const shader::BindingId dirsBinding   = _program.getUniformBinding("dirs");
    const shader::BindingId colorsBinding = _program.getUniformBinding("colors");
    const shader::BindingId paramsBinding = _program.getUniformBinding("params");
    const shader::BindingId spotDirBinding = _program.getUniformBinding("spotDir");
    if (dirsBinding   != shader::InvalidBinding
        && colorsBinding != shader::InvalidBinding
        && paramsBinding != shader::InvalidBinding
        && spotDirBinding != shader::InvalidBinding) {
        // §P5.5 B (2026-07-23) — field-split path: 4 separate
        // `setUniform` calls, each writing one vec4 array.
        _program.setUniform(dirsBinding,   lightsBlock,
                            ayt::render::kMaxSceneLights * 4u * sizeof(float));
        _program.setUniform(colorsBinding,
                            lightsBlock + ayt::render::kMaxSceneLights * 4u,
                            ayt::render::kMaxSceneLights * 4u * sizeof(float));
        _program.setUniform(paramsBinding,
                            lightsBlock + ayt::render::kMaxSceneLights * 8u,
                            ayt::render::kMaxSceneLights * 4u * sizeof(float));
        _program.setUniform(spotDirBinding,
                            lightsBlock + ayt::render::kMaxSceneLights * 12u,
                            ayt::render::kMaxSceneLights * 4u * sizeof(float));
        static bool s_loggedSplit = false;
        if (!s_loggedSplit) {
            s_loggedSplit = true;
            std::fprintf(stderr,
                         "[LightingPass] lights via field uniforms "
                         "dirs+colors+params+spotDir (HLSL/bgfx, "
                         "§P5.5 B field-split path)\n");
        }
    } else {
        // ABORT: the program should have all 4 arrays — Phoskia
        // §P5.5 B source declares them as top-level uniformblock
        // fields. If any binding is missing, the cache key was bumped
        // without updating the Phoskia source (or vice versa). Loud
        // fail so we don't silently ship a black image.
        static bool s_loggedMissing = false;
        if (!s_loggedMissing) {
            s_loggedMissing = true;
            std::fprintf(stderr,
                         "[LightingPass] FATAL: missing one of "
                         "dirs/colors/params/spotDir uniforms — "
                         "check Phoskia source vs cache-key bump\n");
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

    // §P5 B5.5 v15 — worldPos comes from gbufferMotion (RT2), already
    // bound above with the other GBuffer color attachments. No depth
    // reconstruct / u_depthToClip / gbufferDepth bind.

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