#include "detail/LightingPass.h"

#include "detail/BgfxMatrix.h"
#include "detail/FrameContext.h"
#include "detail/FgResource.h"
#include "detail/GBufferPass.h"
#include "detail/GpuResources.h"
#include "detail/RenderPass.h"
#include "detail/RenderResourceBlackboard.h"
#include "detail/SceneLighting.h"
#include "detail/ShadowPass.h"
#include "detail/SSAOPass.h"

#include "AYRenderer/RenderScene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ayt::render::detail
{

// �P5 B5 (2026-07-22) ??build stamp literal (mirror
// GBufferPass.cpp:15 `kGBufferBuildStamp`). Pointer-equal compare;
// bumping this triggers a FBO rebuild on next execute(). B5 is
// the first lock ??bumping is safe across B5.x cuts.
static constexpr const char* kLightingBuildStamp = "lighting-hdr-v1-2026-08-28";
const char* const kLightingBuildStampCStr = kLightingBuildStamp;

// �P5 B5 (2026-07-22) ??cache key literal (mirror GBufferPass.cpp:68
// `kGBufferCacheKey`). Pointer-equal compare so cache invalidates
// when the literal bumps. `s_acquiredCacheKey` static guard inside
// ensureProgram() forces re-acquire.
// �P5 B7+ (2026-07-22) ??bump to v4: Phoskia source now declares a
// `uniformblock Lights { vec4 dirs[8]; vec4 colors[8]; } binding 0`
// and the FS unrolls 8 directional-light taps unconditionally.
// Single-light fallback path through u_lightDirection +
// u_lightColor still exists (no-op on the data path; both uniforms
// still ship) so existing setDirectionalLight host code stays
// compatible.
// �P5 B7+ (2026-07-22) ??v6: Lights UBO must be program-top-level.
// AYBGFXConverter only emits cbuffer from IRProgram::uniformBlocks;
// nested material uniformblocks never reach the HLSL preamble
// (`undeclared identifier 'Lights'` / D3D X3004).
// �P5 B5.5 (2026-07-22) ??bump on top of v11:
//   + `texture2d gbufferWorldPosition` carries worldPos (RT2;
//     GBufferFill v6) ??Lighting decodes instead of sampling D24S8
//   + `texture2d shadowMap` + 4 shadow uniforms (u_lightViewProj,
//     shadowBias, shadowMapTexel, shadowPcf)
//   + FS key-light only PCF (9 taps) ??fill / rim lights
//     (lights[1..7]) stay unshadowed.
//
// Single shared shadow map for the key light (cutsheet �10
// pass-lessons-from-deferred.md:300 ??"LightingPass ??
// ShadowPass ????"). Per-light shadow maps is a separate
// future cut.
// �P5 B5.5 (2026-07-22) ??v16: worldPos from GBuffer RT2 as
// RGBA16F raw xyz (no 8-bit encode). v15 RGBA8 encode caused
// ~0.16m quantization ??mosaic shadow blocks on the ground.
// �P5.5 A (2026-07-23) ??v19: keep UBO field name `dirs` (not `record`).
 // `record` as a bgfx/HLSL uniform identifier was a bad rename ??host
 // upload / reflection mismatch left light vectors uninitialized
 // (flickering cyan fill, unstable key, missing shadows) while
 // `colors[]` still updated. `.w` still carries float(LightType).
 // �P5.5 B (2026-07-23) ??bump to v21: Light POD widened with
 // Point/Spot per-light params (range/intensity/coneCosInner/
 // coneCosOuter) + spotDirection; Phoskia FS rewritten with
 // per-type attenuation + cone math (see fragment body below).
 // UBO widened to 4 vec4 arrays (dirs/colors/params/spotDir).
 // Bug fix #3 (Test_B5 cache-key false-green) is resolved by
 // `kLightingCacheKeyCStr` extern declared in LightingPass.h ??
 // see definition below.
//
// �P5.5 D (2026-07-23) ??bump v21 ??v22: IBL MVP (Ambient Diffuse
// Cube Lookup). Phoskia source declares `texturecube envCube` +
// `uniform float cubeActive` (0/1 per-frame gate) +
// `uniform float ambientStrength` (default 0.6). The FS ambient
// term becomes `ambientFlat + ambientCube` where
//   ambientFlat = vec3(0.1, 0.1, 0.1)              // pre-D floor
//   ambientCube = sample(envCube, N).rgb * ambientStrength * cubeActive
// When cubeActive=0 (host never called Renderer::setSkySourceCube
// OR SkySource::kind != CubeMap) the cube branch contributes 0 ??
// byte-equivalent to pre-D flat ambient. The single-FS +
// uniform-gating strategy mirrors �Skybox0 SkyboxPass dual-kind
// decision (avoid dual-FS program acquire overhead).
//
// �P5.5 C (2026-07-23) ? per-light shadow atlas + AYShader fixes:
//   v24 uniform T name[N]; v25 mat4 let (not mat4x4); v26 mix(vec2,?)
//   overloads (missing ? float _rectUv ? HLSL .y subscript fail).
static constexpr const char* kLightingCacheKey =
    "lighting_v31_ssao_ambient_only";

// �P5.5 B (2026-07-23) ? Bug fix #3: single source of truth for
// cache-key string equality tests. The extern is declared in
// LightingPass.h; this is its definition.
const char* const kLightingCacheKeyCStr = kLightingCacheKey;

// �P5 B5 (2026-07-22) ? fullscreen-triangle vertex data, duplicated
// from PostProcessPass.cpp:27-33 (private state there ? coupling
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

// �P5 B5 (2026-07-22) ??Phoskia Lighting VS/FS source.
//
// Sampler inputs (cutsheet B5 spec):
//   - gbufferAlbedo : color (RGB = baseColor, A = opacity) from B4b
//   - gbufferNormal : color (RGB = world-space normal encoded [0,1])
//   - gbufferWorldPosition : xyz world position, w packed AO/model
//
// Uniform inputs (cutsheet B5 spec, all from FrameContext):
//   - u_lightDirection : vec3 = -frame.lightDirection (Phoskia wants
//                                          a TO-light vector; we
//                                          negate on the CPU once,
//                                          feed as-is)
//   - u_lightColor     : vec3 = frame.lightColor
//   - u_cameraPos      : vec3 = frame.cameraPosition
//     (reserved for B5.5 specular/Blinn-Phong ??B5 keeps Lambert
//     flat; uploader still emits it so future B5.x can reuse the
//     same uniform binding without shader changes)
//
// Math (Lambert):
//   N = sample(gbufferNormal).xyz * 2.0 - vec3(1,1,1)  // decode [0,1]?[-1,1]
//   L = normalize(u_lightDirection)
//   NdotL = max(dot(N, L), 0.0)
//   ambient = vec3(0.1)                                  // floor term
//   lit = sample(gbufferAlbedo).rgb * (ambient + NdotL * u_lightColor)
//   return vec4(lit, sample(gbufferAlbedo).a)
//
// Phoskia `let` chain uses the same surface as the B4b GBufferFill
// source (verified at PR-F2/PR-F3 ship) ??`let` declarations +
// arithmetic + sample() texture lookups. B5 emits a SINGLE `return`
// (not MRT) so no Phoskia MRT extension needed ??falls back to the
// legacy `return ??gl_FragColor` path (verified at AYShader/
// unittest/Test_MRT_Fragment.cpp::mrt_legacy_return_still_emits_fragcolor).
//
// �P5.5 B (2026-07-23) ??Phoskia source rewritten for Point + Spot
// per-type math. UBO `Lights` widened from 2 vec4 arrays to 4
// (`dirs[8]` + `colors[8]` + `params[8]` + `spotDir[8]`). Each of
// the 8 light slots dispatches per-type inside an `if/else if`
// chain (Phoskia `fn` support not relied on ??keeps the converter
// path simple). `params[]` carries (range, intensity, coneCosInner,
// coneCosOuter); `spotDir[].xyz` carries Spot direction (Point /
// Directional leave it zero ??never read by their FS branches).
//
// Branch taxonomy (mirrors cutsheet �P5.5 B):
//   - Directional (w < 0.5):  NdotL * shadow (key only) * color
//   - Point (0.5 ??w < 1.5):  NdotL * (intensity / max(dist�, 0.01))
//                              * (1 - smoothstep(0, range, dist))
//                              * color
//   - Spot (w ??1.5):         same as Point PLUS
//                              smoothstep(coneCosOuter, coneCosInner, cos?)
//                              where cos? = dot(L_to_frag, spotDir)
//
// Shadow attenuation: ONLY lights[0] (key) gets shadowKey multiply,
// regardless of LightType. Fill / rim 1..7 stay unshadowed ??mirror
// A's cutsheet �10 contract. If a host puts Point/Spot in slot 0,
// they get shadow attenuation for free (v0 simplification; per-light
// shadow is �P5.5 C scope).
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
    texture2d gbufferWorldPosition
    texture2d gbufferMaterial
    texture2d shadowMap
    texture2d gbufferSky
    texture2d ssaoTexture
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
    uniform vec4 ssaoParams
    // �P5.5 C (2026-07-23) ??per-light shadow atlas bindings.
    // shadowAtlasRects[i] = (u0,v0,u1,v1) in atlas UV [0,1].
    // lightViewProjs[i]   = light-space VP for slot i.
    // shadowBiases[i].x   = per-slot bias override (0 ??use global).
    // perLightShadowCount.x = N (0..7) active slots.
    uniform vec4 shadowAtlasRects[8]
    uniform mat4 lightViewProjs[8]
    uniform vec4 shadowBiases[8]
    uniform vec4 perLightShadowCount
    uniform vec4 activeLightCount
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
        let worldSample = sample(gbufferWorldPosition, baseUv)
        let surface = sample(gbufferMaterial, baseUv)
        let ssaoCenter = sample(ssaoTexture, baseUv)
        let ssaoLeft = sample(ssaoTexture, baseUv + vec2(-ssaoParams.y, 0.0))
        let ssaoRight = sample(ssaoTexture, baseUv + vec2(ssaoParams.y, 0.0))
        let ssaoDown = sample(ssaoTexture, baseUv + vec2(0.0, -ssaoParams.z))
        let ssaoUp = sample(ssaoTexture, baseUv + vec2(0.0, ssaoParams.z))
        let ssaoWeight = ssaoCenter.a + ssaoLeft.a + ssaoRight.a + ssaoDown.a + ssaoUp.a
        let ssaoOcclusion = clamp(
            (ssaoCenter.r * ssaoCenter.a
             + ssaoLeft.r * ssaoLeft.a
             + ssaoRight.r * ssaoRight.a
             + ssaoDown.r * ssaoDown.a
             + ssaoUp.r * ssaoUp.a) / max(ssaoWeight, 1.0),
            0.0,
            1.0)
        let ssaoAmbient = clamp(1.0 - ssaoOcclusion * ssaoParams.x, 0.0, 1.0)
        let decodedN = normalSample.xyz * 2.0 - vec3(1.0, 1.0, 1.0)
        let N = decodedN * (1.0 / max(length(decodedN), 0.0001))
        // �P5.5 D (2026-07-23) ??IBL MVP ambient term. Default
        // cubeActive=0 ??ambient = ambientFlat (pre-D byte-
        // equivalent). When host calls Renderer::setSkySourceCube
        // + SkySource::kind=CubeMap, cubeActive=1 and the cube
        // lookup adds normal-driven diffuse term scaled by
        // ambientStrength (default 0.6).
        let ambientFlat = vec3(0.1, 0.1, 0.1)
        let ambientCube = sample(envCube, N).rgb * ambientStrength.x * cubeActive.x
        let ambient = ambientFlat + ambientCube
        // �P5 B5.5 v16 ??worldPos from GBuffer RT2 (RGBA16F raw xyz).
        let worldPos = worldSample.xyz
        // �P5.5 C (2026-07-23) ??per-light shadow factor helper.
        // Each light slot i computes its own shadow factor by
        // projecting worldPos through lightViewProjs[i] and
        // sampling shadowMap inside shadowAtlasRects[i]. When
        // i >= perLightShadowCount.x the slot has no shadow caster
        // ??shadowFactor_i = 1.0 (no shadow, byte-equivalent to
        // pre-C behavior for fill/rim lights).
        //
        // The helper body is the same 9-tap PCF used pre-C (cut-
        // sheet �P5.5 C :330 keeps the contract). Per-slot bias
        // falls back to the global `shadowBias.x` when 0.
        //
        // K3 invariant: perLightShadowCount.x == 0 ??
        // `let perLightShadow = vec4(1.0)` baseline (set below),
        // `* perLightShadow *` collapses to no-op ??lights[0]'s
        // shadow factor computed from the global `u_lightViewProj`
        // + `shadowBias` + `shadowMap` is byte-equivalent to the
        // pre-C 9-tap key-only path.
        let _world = vec4(worldPos, 1.0)
        let _tx = shadowMapTexel.x
        let _ty = shadowMapTexel.y
        let _globalBias = shadowBias.x

        // ---- per-slot shadow helpers (8 unrolled to avoid fn-call ABI risk) ----
        // slot 0
        let _lvp0 = lightViewProjs[0]
        let _clip0 = _lvp0 * _world
        let _invW0 = 1.0 / max(_clip0.w, 0.0001)
        let _ndcX0 = _clip0.x * _invW0
        let _ndcY0 = _clip0.y * _invW0
        let _ref0 = _clip0.z * _invW0 * 0.5 + 0.5
        let _uy0 = _ndcY0 * 0.5 + 0.5
        let _altUv0 = vec2(_ndcX0 * 0.5 + 0.5, 1.0 - _uy0)
        let _rectUv0 = mix(shadowAtlasRects[0].xy, shadowAtlasRects[0].zw, _altUv0)
        let _safeUv0 = vec2(
            max(shadowAtlasRects[0].x + _tx * 1.5, min(shadowAtlasRects[0].z - _tx * 1.5, _rectUv0.x)),
            max(shadowAtlasRects[0].y + _ty * 1.5, min(shadowAtlasRects[0].w - _ty * 1.5, _rectUv0.y)))
        let _inMap0 = step(0.0001, _clip0.w)
                    * step(0.0, _altUv0.x) * step(_altUv0.x, 1.0)
                    * step(0.0, _altUv0.y) * step(_altUv0.y, 1.0)
                    * step(0.0, _ref0) * step(_ref0, 1.0)
        let _bias0 = shadowBiases[0].x * step(0.0001, shadowBiases[0].x) + _globalBias * (1.0 - step(0.0001, shadowBiases[0].x))
        let _s0a = sample(shadowMap, _safeUv0 + vec2(-_tx, -_ty)).x
        let _s0b = sample(shadowMap, _safeUv0 + vec2(0.0, -_ty)).x
        let _s0c = sample(shadowMap, _safeUv0 + vec2(_tx, -_ty)).x
        let _s0d = sample(shadowMap, _safeUv0 + vec2(-_tx, 0.0)).x
        let _s0e = sample(shadowMap, _safeUv0 + vec2(0.0, 0.0)).x
        let _s0f = sample(shadowMap, _safeUv0 + vec2(_tx, 0.0)).x
        let _s0g = sample(shadowMap, _safeUv0 + vec2(-_tx, _ty)).x
        let _s0h = sample(shadowMap, _safeUv0 + vec2(0.0, _ty)).x
        let _s0i = sample(shadowMap, _safeUv0 + vec2(_tx, _ty)).x
        let _t0a = max(1.0 - step(_s0a + _bias0, _ref0), step(0.999, _s0a))
        let _t0b = max(1.0 - step(_s0b + _bias0, _ref0), step(0.999, _s0b))
        let _t0c = max(1.0 - step(_s0c + _bias0, _ref0), step(0.999, _s0c))
        let _t0d = max(1.0 - step(_s0d + _bias0, _ref0), step(0.999, _s0d))
        let _t0e = max(1.0 - step(_s0e + _bias0, _ref0), step(0.999, _s0e))
        let _t0f = max(1.0 - step(_s0f + _bias0, _ref0), step(0.999, _s0f))
        let _t0g = max(1.0 - step(_s0g + _bias0, _ref0), step(0.999, _s0g))
        let _t0h = max(1.0 - step(_s0h + _bias0, _ref0), step(0.999, _s0h))
        let _t0i = max(1.0 - step(_s0i + _bias0, _ref0), step(0.999, _s0i))
        let _soft0 = (_t0a + _t0b + _t0c + _t0d + _t0e + _t0f + _t0g + _t0h + _t0i) * (1.0 / 9.0)
        let _shadow0 = mix(1.0, mix(_t0e, _soft0, shadowPcf.x), _inMap0)
        let perLightShadow0 = mix(vec4(1.0), vec4(_shadow0), step(0.5, perLightShadowCount.x))

        // slot 1
        let _lvp1 = lightViewProjs[1]
        let _clip1 = _lvp1 * _world
        let _invW1 = 1.0 / max(_clip1.w, 0.0001)
        let _ndcX1 = _clip1.x * _invW1
        let _ndcY1 = _clip1.y * _invW1
        let _ref1 = _clip1.z * _invW1 * 0.5 + 0.5
        let _uy1 = _ndcY1 * 0.5 + 0.5
        let _altUv1 = vec2(_ndcX1 * 0.5 + 0.5, 1.0 - _uy1)
        let _rectUv1 = mix(shadowAtlasRects[1].xy, shadowAtlasRects[1].zw, _altUv1)
        let _safeUv1 = vec2(
            max(shadowAtlasRects[1].x + _tx * 1.5, min(shadowAtlasRects[1].z - _tx * 1.5, _rectUv1.x)),
            max(shadowAtlasRects[1].y + _ty * 1.5, min(shadowAtlasRects[1].w - _ty * 1.5, _rectUv1.y)))
        let _inMap1 = step(0.0001, _clip1.w)
                    * step(0.0, _altUv1.x) * step(_altUv1.x, 1.0)
                    * step(0.0, _altUv1.y) * step(_altUv1.y, 1.0)
                    * step(0.0, _ref1) * step(_ref1, 1.0)
        let _bias1 = shadowBiases[1].x * step(0.0001, shadowBiases[1].x) + _globalBias * (1.0 - step(0.0001, shadowBiases[1].x))
        let _s1a = sample(shadowMap, _safeUv1 + vec2(-_tx, -_ty)).x
        let _s1b = sample(shadowMap, _safeUv1 + vec2(0.0, -_ty)).x
        let _s1c = sample(shadowMap, _safeUv1 + vec2(_tx, -_ty)).x
        let _s1d = sample(shadowMap, _safeUv1 + vec2(-_tx, 0.0)).x
        let _s1e = sample(shadowMap, _safeUv1 + vec2(0.0, 0.0)).x
        let _s1f = sample(shadowMap, _safeUv1 + vec2(_tx, 0.0)).x
        let _s1g = sample(shadowMap, _safeUv1 + vec2(-_tx, _ty)).x
        let _s1h = sample(shadowMap, _safeUv1 + vec2(0.0, _ty)).x
        let _s1i = sample(shadowMap, _safeUv1 + vec2(_tx, _ty)).x
        let _t1a = max(1.0 - step(_s1a + _bias1, _ref1), step(0.999, _s1a))
        let _t1b = max(1.0 - step(_s1b + _bias1, _ref1), step(0.999, _s1b))
        let _t1c = max(1.0 - step(_s1c + _bias1, _ref1), step(0.999, _s1c))
        let _t1d = max(1.0 - step(_s1d + _bias1, _ref1), step(0.999, _s1d))
        let _t1e = max(1.0 - step(_s1e + _bias1, _ref1), step(0.999, _s1e))
        let _t1f = max(1.0 - step(_s1f + _bias1, _ref1), step(0.999, _s1f))
        let _t1g = max(1.0 - step(_s1g + _bias1, _ref1), step(0.999, _s1g))
        let _t1h = max(1.0 - step(_s1h + _bias1, _ref1), step(0.999, _s1h))
        let _t1i = max(1.0 - step(_s1i + _bias1, _ref1), step(0.999, _s1i))
        let _soft1 = (_t1a + _t1b + _t1c + _t1d + _t1e + _t1f + _t1g + _t1h + _t1i) * (1.0 / 9.0)
        let _shadow1 = mix(1.0, mix(_t1e, _soft1, shadowPcf.x), _inMap1)
        let perLightShadow1 = mix(vec4(1.0), vec4(_shadow1), step(1.5, perLightShadowCount.x))

        // slot 2
        let _lvp2 = lightViewProjs[2]
        let _clip2 = _lvp2 * _world
        let _invW2 = 1.0 / max(_clip2.w, 0.0001)
        let _ndcX2 = _clip2.x * _invW2
        let _ndcY2 = _clip2.y * _invW2
        let _ref2 = _clip2.z * _invW2 * 0.5 + 0.5
        let _uy2 = _ndcY2 * 0.5 + 0.5
        let _altUv2 = vec2(_ndcX2 * 0.5 + 0.5, 1.0 - _uy2)
        let _rectUv2 = mix(shadowAtlasRects[2].xy, shadowAtlasRects[2].zw, _altUv2)
        let _safeUv2 = vec2(
            max(shadowAtlasRects[2].x + _tx * 1.5, min(shadowAtlasRects[2].z - _tx * 1.5, _rectUv2.x)),
            max(shadowAtlasRects[2].y + _ty * 1.5, min(shadowAtlasRects[2].w - _ty * 1.5, _rectUv2.y)))
        let _inMap2 = step(0.0001, _clip2.w)
                    * step(0.0, _altUv2.x) * step(_altUv2.x, 1.0)
                    * step(0.0, _altUv2.y) * step(_altUv2.y, 1.0)
                    * step(0.0, _ref2) * step(_ref2, 1.0)
        let _bias2 = shadowBiases[2].x * step(0.0001, shadowBiases[2].x) + _globalBias * (1.0 - step(0.0001, shadowBiases[2].x))
        let _s2a = sample(shadowMap, _safeUv2 + vec2(-_tx, -_ty)).x
        let _s2b = sample(shadowMap, _safeUv2 + vec2(0.0, -_ty)).x
        let _s2c = sample(shadowMap, _safeUv2 + vec2(_tx, -_ty)).x
        let _s2d = sample(shadowMap, _safeUv2 + vec2(-_tx, 0.0)).x
        let _s2e = sample(shadowMap, _safeUv2 + vec2(0.0, 0.0)).x
        let _s2f = sample(shadowMap, _safeUv2 + vec2(_tx, 0.0)).x
        let _s2g = sample(shadowMap, _safeUv2 + vec2(-_tx, _ty)).x
        let _s2h = sample(shadowMap, _safeUv2 + vec2(0.0, _ty)).x
        let _s2i = sample(shadowMap, _safeUv2 + vec2(_tx, _ty)).x
        let _t2a = max(1.0 - step(_s2a + _bias2, _ref2), step(0.999, _s2a))
        let _t2b = max(1.0 - step(_s2b + _bias2, _ref2), step(0.999, _s2b))
        let _t2c = max(1.0 - step(_s2c + _bias2, _ref2), step(0.999, _s2c))
        let _t2d = max(1.0 - step(_s2d + _bias2, _ref2), step(0.999, _s2d))
        let _t2e = max(1.0 - step(_s2e + _bias2, _ref2), step(0.999, _s2e))
        let _t2f = max(1.0 - step(_s2f + _bias2, _ref2), step(0.999, _s2f))
        let _t2g = max(1.0 - step(_s2g + _bias2, _ref2), step(0.999, _s2g))
        let _t2h = max(1.0 - step(_s2h + _bias2, _ref2), step(0.999, _s2h))
        let _t2i = max(1.0 - step(_s2i + _bias2, _ref2), step(0.999, _s2i))
        let _soft2 = (_t2a + _t2b + _t2c + _t2d + _t2e + _t2f + _t2g + _t2h + _t2i) * (1.0 / 9.0)
        let _shadow2 = mix(1.0, mix(_t2e, _soft2, shadowPcf.x), _inMap2)
        let perLightShadow2 = mix(vec4(1.0), vec4(_shadow2), step(2.5, perLightShadowCount.x))

        // slot 3
        let _lvp3 = lightViewProjs[3]
        let _clip3 = _lvp3 * _world
        let _invW3 = 1.0 / max(_clip3.w, 0.0001)
        let _ndcX3 = _clip3.x * _invW3
        let _ndcY3 = _clip3.y * _invW3
        let _ref3 = _clip3.z * _invW3 * 0.5 + 0.5
        let _uy3 = _ndcY3 * 0.5 + 0.5
        let _altUv3 = vec2(_ndcX3 * 0.5 + 0.5, 1.0 - _uy3)
        let _rectUv3 = mix(shadowAtlasRects[3].xy, shadowAtlasRects[3].zw, _altUv3)
        let _safeUv3 = vec2(
            max(shadowAtlasRects[3].x + _tx * 1.5, min(shadowAtlasRects[3].z - _tx * 1.5, _rectUv3.x)),
            max(shadowAtlasRects[3].y + _ty * 1.5, min(shadowAtlasRects[3].w - _ty * 1.5, _rectUv3.y)))
        let _inMap3 = step(0.0001, _clip3.w)
                    * step(0.0, _altUv3.x) * step(_altUv3.x, 1.0)
                    * step(0.0, _altUv3.y) * step(_altUv3.y, 1.0)
                    * step(0.0, _ref3) * step(_ref3, 1.0)
        let _bias3 = shadowBiases[3].x * step(0.0001, shadowBiases[3].x) + _globalBias * (1.0 - step(0.0001, shadowBiases[3].x))
        let _s3a = sample(shadowMap, _safeUv3 + vec2(-_tx, -_ty)).x
        let _s3b = sample(shadowMap, _safeUv3 + vec2(0.0, -_ty)).x
        let _s3c = sample(shadowMap, _safeUv3 + vec2(_tx, -_ty)).x
        let _s3d = sample(shadowMap, _safeUv3 + vec2(-_tx, 0.0)).x
        let _s3e = sample(shadowMap, _safeUv3 + vec2(0.0, 0.0)).x
        let _s3f = sample(shadowMap, _safeUv3 + vec2(_tx, 0.0)).x
        let _s3g = sample(shadowMap, _safeUv3 + vec2(-_tx, _ty)).x
        let _s3h = sample(shadowMap, _safeUv3 + vec2(0.0, _ty)).x
        let _s3i = sample(shadowMap, _safeUv3 + vec2(_tx, _ty)).x
        let _t3a = max(1.0 - step(_s3a + _bias3, _ref3), step(0.999, _s3a))
        let _t3b = max(1.0 - step(_s3b + _bias3, _ref3), step(0.999, _s3b))
        let _t3c = max(1.0 - step(_s3c + _bias3, _ref3), step(0.999, _s3c))
        let _t3d = max(1.0 - step(_s3d + _bias3, _ref3), step(0.999, _s3d))
        let _t3e = max(1.0 - step(_s3e + _bias3, _ref3), step(0.999, _s3e))
        let _t3f = max(1.0 - step(_s3f + _bias3, _ref3), step(0.999, _s3f))
        let _t3g = max(1.0 - step(_s3g + _bias3, _ref3), step(0.999, _s3g))
        let _t3h = max(1.0 - step(_s3h + _bias3, _ref3), step(0.999, _s3h))
        let _t3i = max(1.0 - step(_s3i + _bias3, _ref3), step(0.999, _s3i))
        let _soft3 = (_t3a + _t3b + _t3c + _t3d + _t3e + _t3f + _t3g + _t3h + _t3i) * (1.0 / 9.0)
        let _shadow3 = mix(1.0, mix(_t3e, _soft3, shadowPcf.x), _inMap3)
        let perLightShadow3 = mix(vec4(1.0), vec4(_shadow3), step(3.5, perLightShadowCount.x))

        // slot 4
        let _lvp4 = lightViewProjs[4]
        let _clip4 = _lvp4 * _world
        let _invW4 = 1.0 / max(_clip4.w, 0.0001)
        let _ndcX4 = _clip4.x * _invW4
        let _ndcY4 = _clip4.y * _invW4
        let _ref4 = _clip4.z * _invW4 * 0.5 + 0.5
        let _uy4 = _ndcY4 * 0.5 + 0.5
        let _altUv4 = vec2(_ndcX4 * 0.5 + 0.5, 1.0 - _uy4)
        let _rectUv4 = mix(shadowAtlasRects[4].xy, shadowAtlasRects[4].zw, _altUv4)
        let _safeUv4 = vec2(
            max(shadowAtlasRects[4].x + _tx * 1.5, min(shadowAtlasRects[4].z - _tx * 1.5, _rectUv4.x)),
            max(shadowAtlasRects[4].y + _ty * 1.5, min(shadowAtlasRects[4].w - _ty * 1.5, _rectUv4.y)))
        let _inMap4 = step(0.0001, _clip4.w)
                    * step(0.0, _altUv4.x) * step(_altUv4.x, 1.0)
                    * step(0.0, _altUv4.y) * step(_altUv4.y, 1.0)
                    * step(0.0, _ref4) * step(_ref4, 1.0)
        let _bias4 = shadowBiases[4].x * step(0.0001, shadowBiases[4].x) + _globalBias * (1.0 - step(0.0001, shadowBiases[4].x))
        let _s4a = sample(shadowMap, _safeUv4 + vec2(-_tx, -_ty)).x
        let _s4b = sample(shadowMap, _safeUv4 + vec2(0.0, -_ty)).x
        let _s4c = sample(shadowMap, _safeUv4 + vec2(_tx, -_ty)).x
        let _s4d = sample(shadowMap, _safeUv4 + vec2(-_tx, 0.0)).x
        let _s4e = sample(shadowMap, _safeUv4 + vec2(0.0, 0.0)).x
        let _s4f = sample(shadowMap, _safeUv4 + vec2(_tx, 0.0)).x
        let _s4g = sample(shadowMap, _safeUv4 + vec2(-_tx, _ty)).x
        let _s4h = sample(shadowMap, _safeUv4 + vec2(0.0, _ty)).x
        let _s4i = sample(shadowMap, _safeUv4 + vec2(_tx, _ty)).x
        let _t4a = max(1.0 - step(_s4a + _bias4, _ref4), step(0.999, _s4a))
        let _t4b = max(1.0 - step(_s4b + _bias4, _ref4), step(0.999, _s4b))
        let _t4c = max(1.0 - step(_s4c + _bias4, _ref4), step(0.999, _s4c))
        let _t4d = max(1.0 - step(_s4d + _bias4, _ref4), step(0.999, _s4d))
        let _t4e = max(1.0 - step(_s4e + _bias4, _ref4), step(0.999, _s4e))
        let _t4f = max(1.0 - step(_s4f + _bias4, _ref4), step(0.999, _s4f))
        let _t4g = max(1.0 - step(_s4g + _bias4, _ref4), step(0.999, _s4g))
        let _t4h = max(1.0 - step(_s4h + _bias4, _ref4), step(0.999, _s4h))
        let _t4i = max(1.0 - step(_s4i + _bias4, _ref4), step(0.999, _s4i))
        let _soft4 = (_t4a + _t4b + _t4c + _t4d + _t4e + _t4f + _t4g + _t4h + _t4i) * (1.0 / 9.0)
        let _shadow4 = mix(1.0, mix(_t4e, _soft4, shadowPcf.x), _inMap4)
        let perLightShadow4 = mix(vec4(1.0), vec4(_shadow4), step(4.5, perLightShadowCount.x))

        // slot 5
        let _lvp5 = lightViewProjs[5]
        let _clip5 = _lvp5 * _world
        let _invW5 = 1.0 / max(_clip5.w, 0.0001)
        let _ndcX5 = _clip5.x * _invW5
        let _ndcY5 = _clip5.y * _invW5
        let _ref5 = _clip5.z * _invW5 * 0.5 + 0.5
        let _uy5 = _ndcY5 * 0.5 + 0.5
        let _altUv5 = vec2(_ndcX5 * 0.5 + 0.5, 1.0 - _uy5)
        let _rectUv5 = mix(shadowAtlasRects[5].xy, shadowAtlasRects[5].zw, _altUv5)
        let _safeUv5 = vec2(
            max(shadowAtlasRects[5].x + _tx * 1.5, min(shadowAtlasRects[5].z - _tx * 1.5, _rectUv5.x)),
            max(shadowAtlasRects[5].y + _ty * 1.5, min(shadowAtlasRects[5].w - _ty * 1.5, _rectUv5.y)))
        let _inMap5 = step(0.0001, _clip5.w)
                    * step(0.0, _altUv5.x) * step(_altUv5.x, 1.0)
                    * step(0.0, _altUv5.y) * step(_altUv5.y, 1.0)
                    * step(0.0, _ref5) * step(_ref5, 1.0)
        let _bias5 = shadowBiases[5].x * step(0.0001, shadowBiases[5].x) + _globalBias * (1.0 - step(0.0001, shadowBiases[5].x))
        let _s5a = sample(shadowMap, _safeUv5 + vec2(-_tx, -_ty)).x
        let _s5b = sample(shadowMap, _safeUv5 + vec2(0.0, -_ty)).x
        let _s5c = sample(shadowMap, _safeUv5 + vec2(_tx, -_ty)).x
        let _s5d = sample(shadowMap, _safeUv5 + vec2(-_tx, 0.0)).x
        let _s5e = sample(shadowMap, _safeUv5 + vec2(0.0, 0.0)).x
        let _s5f = sample(shadowMap, _safeUv5 + vec2(_tx, 0.0)).x
        let _s5g = sample(shadowMap, _safeUv5 + vec2(-_tx, _ty)).x
        let _s5h = sample(shadowMap, _safeUv5 + vec2(0.0, _ty)).x
        let _s5i = sample(shadowMap, _safeUv5 + vec2(_tx, _ty)).x
        let _t5a = max(1.0 - step(_s5a + _bias5, _ref5), step(0.999, _s5a))
        let _t5b = max(1.0 - step(_s5b + _bias5, _ref5), step(0.999, _s5b))
        let _t5c = max(1.0 - step(_s5c + _bias5, _ref5), step(0.999, _s5c))
        let _t5d = max(1.0 - step(_s5d + _bias5, _ref5), step(0.999, _s5d))
        let _t5e = max(1.0 - step(_s5e + _bias5, _ref5), step(0.999, _s5e))
        let _t5f = max(1.0 - step(_s5f + _bias5, _ref5), step(0.999, _s5f))
        let _t5g = max(1.0 - step(_s5g + _bias5, _ref5), step(0.999, _s5g))
        let _t5h = max(1.0 - step(_s5h + _bias5, _ref5), step(0.999, _s5h))
        let _t5i = max(1.0 - step(_s5i + _bias5, _ref5), step(0.999, _s5i))
        let _soft5 = (_t5a + _t5b + _t5c + _t5d + _t5e + _t5f + _t5g + _t5h + _t5i) * (1.0 / 9.0)
        let _shadow5 = mix(1.0, mix(_t5e, _soft5, shadowPcf.x), _inMap5)
        let perLightShadow5 = mix(vec4(1.0), vec4(_shadow5), step(5.5, perLightShadowCount.x))

        // slot 6
        let _lvp6 = lightViewProjs[6]
        let _clip6 = _lvp6 * _world
        let _invW6 = 1.0 / max(_clip6.w, 0.0001)
        let _ndcX6 = _clip6.x * _invW6
        let _ndcY6 = _clip6.y * _invW6
        let _ref6 = _clip6.z * _invW6 * 0.5 + 0.5
        let _uy6 = _ndcY6 * 0.5 + 0.5
        let _altUv6 = vec2(_ndcX6 * 0.5 + 0.5, 1.0 - _uy6)
        let _rectUv6 = mix(shadowAtlasRects[6].xy, shadowAtlasRects[6].zw, _altUv6)
        let _safeUv6 = vec2(
            max(shadowAtlasRects[6].x + _tx * 1.5, min(shadowAtlasRects[6].z - _tx * 1.5, _rectUv6.x)),
            max(shadowAtlasRects[6].y + _ty * 1.5, min(shadowAtlasRects[6].w - _ty * 1.5, _rectUv6.y)))
        let _inMap6 = step(0.0001, _clip6.w)
                    * step(0.0, _altUv6.x) * step(_altUv6.x, 1.0)
                    * step(0.0, _altUv6.y) * step(_altUv6.y, 1.0)
                    * step(0.0, _ref6) * step(_ref6, 1.0)
        let _bias6 = shadowBiases[6].x * step(0.0001, shadowBiases[6].x) + _globalBias * (1.0 - step(0.0001, shadowBiases[6].x))
        let _s6a = sample(shadowMap, _safeUv6 + vec2(-_tx, -_ty)).x
        let _s6b = sample(shadowMap, _safeUv6 + vec2(0.0, -_ty)).x
        let _s6c = sample(shadowMap, _safeUv6 + vec2(_tx, -_ty)).x
        let _s6d = sample(shadowMap, _safeUv6 + vec2(-_tx, 0.0)).x
        let _s6e = sample(shadowMap, _safeUv6 + vec2(0.0, 0.0)).x
        let _s6f = sample(shadowMap, _safeUv6 + vec2(_tx, 0.0)).x
        let _s6g = sample(shadowMap, _safeUv6 + vec2(-_tx, _ty)).x
        let _s6h = sample(shadowMap, _safeUv6 + vec2(0.0, _ty)).x
        let _s6i = sample(shadowMap, _safeUv6 + vec2(_tx, _ty)).x
        let _t6a = max(1.0 - step(_s6a + _bias6, _ref6), step(0.999, _s6a))
        let _t6b = max(1.0 - step(_s6b + _bias6, _ref6), step(0.999, _s6b))
        let _t6c = max(1.0 - step(_s6c + _bias6, _ref6), step(0.999, _s6c))
        let _t6d = max(1.0 - step(_s6d + _bias6, _ref6), step(0.999, _s6d))
        let _t6e = max(1.0 - step(_s6e + _bias6, _ref6), step(0.999, _s6e))
        let _t6f = max(1.0 - step(_s6f + _bias6, _ref6), step(0.999, _s6f))
        let _t6g = max(1.0 - step(_s6g + _bias6, _ref6), step(0.999, _s6g))
        let _t6h = max(1.0 - step(_s6h + _bias6, _ref6), step(0.999, _s6h))
        let _t6i = max(1.0 - step(_s6i + _bias6, _ref6), step(0.999, _s6i))
        let _soft6 = (_t6a + _t6b + _t6c + _t6d + _t6e + _t6f + _t6g + _t6h + _t6i) * (1.0 / 9.0)
        let _shadow6 = mix(1.0, mix(_t6e, _soft6, shadowPcf.x), _inMap6)
        let perLightShadow6 = mix(vec4(1.0), vec4(_shadow6), step(6.5, perLightShadowCount.x))

        // slot 7
        let _lvp7 = lightViewProjs[7]
        let _clip7 = _lvp7 * _world
        let _invW7 = 1.0 / max(_clip7.w, 0.0001)
        let _ndcX7 = _clip7.x * _invW7
        let _ndcY7 = _clip7.y * _invW7
        let _ref7 = _clip7.z * _invW7 * 0.5 + 0.5
        let _uy7 = _ndcY7 * 0.5 + 0.5
        let _altUv7 = vec2(_ndcX7 * 0.5 + 0.5, 1.0 - _uy7)
        let _rectUv7 = mix(shadowAtlasRects[7].xy, shadowAtlasRects[7].zw, _altUv7)
        let _safeUv7 = vec2(
            max(shadowAtlasRects[7].x + _tx * 1.5, min(shadowAtlasRects[7].z - _tx * 1.5, _rectUv7.x)),
            max(shadowAtlasRects[7].y + _ty * 1.5, min(shadowAtlasRects[7].w - _ty * 1.5, _rectUv7.y)))
        let _inMap7 = step(0.0001, _clip7.w)
                    * step(0.0, _altUv7.x) * step(_altUv7.x, 1.0)
                    * step(0.0, _altUv7.y) * step(_altUv7.y, 1.0)
                    * step(0.0, _ref7) * step(_ref7, 1.0)
        let _bias7 = shadowBiases[7].x * step(0.0001, shadowBiases[7].x) + _globalBias * (1.0 - step(0.0001, shadowBiases[7].x))
        let _s7a = sample(shadowMap, _safeUv7 + vec2(-_tx, -_ty)).x
        let _s7b = sample(shadowMap, _safeUv7 + vec2(0.0, -_ty)).x
        let _s7c = sample(shadowMap, _safeUv7 + vec2(_tx, -_ty)).x
        let _s7d = sample(shadowMap, _safeUv7 + vec2(-_tx, 0.0)).x
        let _s7e = sample(shadowMap, _safeUv7 + vec2(0.0, 0.0)).x
        let _s7f = sample(shadowMap, _safeUv7 + vec2(_tx, 0.0)).x
        let _s7g = sample(shadowMap, _safeUv7 + vec2(-_tx, _ty)).x
        let _s7h = sample(shadowMap, _safeUv7 + vec2(0.0, _ty)).x
        let _s7i = sample(shadowMap, _safeUv7 + vec2(_tx, _ty)).x
        let _t7a = max(1.0 - step(_s7a + _bias7, _ref7), step(0.999, _s7a))
        let _t7b = max(1.0 - step(_s7b + _bias7, _ref7), step(0.999, _s7b))
        let _t7c = max(1.0 - step(_s7c + _bias7, _ref7), step(0.999, _s7c))
        let _t7d = max(1.0 - step(_s7d + _bias7, _ref7), step(0.999, _s7d))
        let _t7e = max(1.0 - step(_s7e + _bias7, _ref7), step(0.999, _s7e))
        let _t7f = max(1.0 - step(_s7f + _bias7, _ref7), step(0.999, _s7f))
        let _t7g = max(1.0 - step(_s7g + _bias7, _ref7), step(0.999, _s7g))
        let _t7h = max(1.0 - step(_s7h + _bias7, _ref7), step(0.999, _s7h))
        let _t7i = max(1.0 - step(_s7i + _bias7, _ref7), step(0.999, _s7i))
        let _soft7 = (_t7a + _t7b + _t7c + _t7d + _t7e + _t7f + _t7g + _t7h + _t7i) * (1.0 / 9.0)
        let _shadow7 = mix(1.0, mix(_t7e, _soft7, shadowPcf.x), _inMap7)
        let perLightShadow7 = mix(vec4(1.0), vec4(_shadow7), step(7.5, perLightShadowCount.x))

        // �P5.5 B (2026-07-23) ??per-light contribution. Each light
        // dispatches per-type via `dirs[i].w = float(LightType)`:
        //   - Directional: NdotL * shadow (per-slot) * color
        //   - Point:       NdotL * intensity / dist� * range-falloff * color
        //   - Spot:        Point path * cone (smoothstep coneCosOuter?inner) * color
        //
        // �P5.5 C (2026-07-23) ??shadow multiply now per-light via
        // perLightShadow{i}.x. When perLightShadowCount.x = 0 the
        // mix collapses perLightShadow{i} = vec4(1.0) for all slots
        // ??pre-C byte-equivalent (lights[0] still uses the global
        // `shadowKey` via u_lightViewProj + global shadowBias as a
        // separate path; see `keyContrib` below).
        //
        // --- light 0 (key) ---
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
        // Legacy and atlas producers both publish slot 0 through the same
        // array contract, so there is only one shadow sampling path.
        let _keyShadow = perLightShadow0.x
        let dirPart0  = NdotDir0 * _keyShadow * Lights.colors[0].xyz
        let _atlasShadow0 = perLightShadow0.x
        let pointPart0 = NdotPos0 * attenPoint0 * _atlasShadow0 * Lights.colors[0].xyz
        let spotPart0  = NdotPos0 * attenSpot0  * _atlasShadow0 * Lights.colors[0].xyz
        let isDir0  = 1.0 - step(0.5, Lights.dirs[0].w)
        let isPoint0 = step(0.5, Lights.dirs[0].w) - step(1.5, Lights.dirs[0].w)
        let isSpot0 = step(1.5, Lights.dirs[0].w)
        let active0 = step(0.5, activeLightCount.x)
        let keyContrib = (dirPart0 * isDir0 + pointPart0 * isPoint0 + spotPart0 * isSpot0) * active0
        // --- lights 1..7 (fill/rim ??per-slot shadow via perLightShadow{i}) ---
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
        let dirPart1 = NdotDir1 * perLightShadow1.x * Lights.colors[1].xyz
        let pointPart1 = NdotPos1 * attenPoint1 * perLightShadow1.x * Lights.colors[1].xyz
        let spotPart1 = NdotPos1 * attenSpot1 * perLightShadow1.x * Lights.colors[1].xyz
        let isDir1 = 1.0 - step(0.5, Lights.dirs[1].w)
        let isPoint1 = step(0.5, Lights.dirs[1].w) - step(1.5, Lights.dirs[1].w)
        let isSpot1 = step(1.5, Lights.dirs[1].w)
        let active1 = step(1.5, activeLightCount.x)
        let f1 = (dirPart1 * isDir1 + pointPart1 * isPoint1 + spotPart1 * isSpot1) * active1
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
        let dirPart2 = NdotDir2 * perLightShadow2.x * Lights.colors[2].xyz
        let pointPart2 = NdotPos2 * attenPoint2 * perLightShadow2.x * Lights.colors[2].xyz
        let spotPart2 = NdotPos2 * attenSpot2 * perLightShadow2.x * Lights.colors[2].xyz
        let isDir2 = 1.0 - step(0.5, Lights.dirs[2].w)
        let isPoint2 = step(0.5, Lights.dirs[2].w) - step(1.5, Lights.dirs[2].w)
        let isSpot2 = step(1.5, Lights.dirs[2].w)
        let active2 = step(2.5, activeLightCount.x)
        let f2 = (dirPart2 * isDir2 + pointPart2 * isPoint2 + spotPart2 * isSpot2) * active2
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
        let dirPart3 = NdotDir3 * perLightShadow3.x * Lights.colors[3].xyz
        let pointPart3 = NdotPos3 * attenPoint3 * perLightShadow3.x * Lights.colors[3].xyz
        let spotPart3 = NdotPos3 * attenSpot3 * perLightShadow3.x * Lights.colors[3].xyz
        let isDir3 = 1.0 - step(0.5, Lights.dirs[3].w)
        let isPoint3 = step(0.5, Lights.dirs[3].w) - step(1.5, Lights.dirs[3].w)
        let isSpot3 = step(1.5, Lights.dirs[3].w)
        let active3 = step(3.5, activeLightCount.x)
        let f3 = (dirPart3 * isDir3 + pointPart3 * isPoint3 + spotPart3 * isSpot3) * active3
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
        let dirPart4 = NdotDir4 * perLightShadow4.x * Lights.colors[4].xyz
        let pointPart4 = NdotPos4 * attenPoint4 * perLightShadow4.x * Lights.colors[4].xyz
        let spotPart4 = NdotPos4 * attenSpot4 * perLightShadow4.x * Lights.colors[4].xyz
        let isDir4 = 1.0 - step(0.5, Lights.dirs[4].w)
        let isPoint4 = step(0.5, Lights.dirs[4].w) - step(1.5, Lights.dirs[4].w)
        let isSpot4 = step(1.5, Lights.dirs[4].w)
        let active4 = step(4.5, activeLightCount.x)
        let f4 = (dirPart4 * isDir4 + pointPart4 * isPoint4 + spotPart4 * isSpot4) * active4
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
        let dirPart5 = NdotDir5 * perLightShadow5.x * Lights.colors[5].xyz
        let pointPart5 = NdotPos5 * attenPoint5 * perLightShadow5.x * Lights.colors[5].xyz
        let spotPart5 = NdotPos5 * attenSpot5 * perLightShadow5.x * Lights.colors[5].xyz
        let isDir5 = 1.0 - step(0.5, Lights.dirs[5].w)
        let isPoint5 = step(0.5, Lights.dirs[5].w) - step(1.5, Lights.dirs[5].w)
        let isSpot5 = step(1.5, Lights.dirs[5].w)
        let active5 = step(5.5, activeLightCount.x)
        let f5 = (dirPart5 * isDir5 + pointPart5 * isPoint5 + spotPart5 * isSpot5) * active5
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
        let dirPart6 = NdotDir6 * perLightShadow6.x * Lights.colors[6].xyz
        let pointPart6 = NdotPos6 * attenPoint6 * perLightShadow6.x * Lights.colors[6].xyz
        let spotPart6 = NdotPos6 * attenSpot6 * perLightShadow6.x * Lights.colors[6].xyz
        let isDir6 = 1.0 - step(0.5, Lights.dirs[6].w)
        let isPoint6 = step(0.5, Lights.dirs[6].w) - step(1.5, Lights.dirs[6].w)
        let isSpot6 = step(1.5, Lights.dirs[6].w)
        let active6 = step(6.5, activeLightCount.x)
        let f6 = (dirPart6 * isDir6 + pointPart6 * isPoint6 + spotPart6 * isSpot6) * active6
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
        let dirPart7 = NdotDir7 * perLightShadow7.x * Lights.colors[7].xyz
        let pointPart7 = NdotPos7 * attenPoint7 * perLightShadow7.x * Lights.colors[7].xyz
        let spotPart7 = NdotPos7 * attenSpot7 * perLightShadow7.x * Lights.colors[7].xyz
        let isDir7 = 1.0 - step(0.5, Lights.dirs[7].w)
        let isPoint7 = step(0.5, Lights.dirs[7].w) - step(1.5, Lights.dirs[7].w)
        let isSpot7 = step(1.5, Lights.dirs[7].w)
        let active7 = step(7.5, activeLightCount.x)
        let f7 = (dirPart7 * isDir7 + pointPart7 * isPoint7 + spotPart7 * isSpot7) * active7
        let materialMetallic = max(0.0, min(1.0, albedo.a))
        let materialRoughness = max(0.045, min(1.0, normalSample.a))
        let materialModel = floor(worldSample.a * 0.5 + 0.0001)
        let materialAo = max(0.0, min(1.0, worldSample.a - materialModel * 2.0))
        let toCamera = u_cameraPos.xyz - worldPos
        let V = toCamera * (1.0 / max(length(toCamera), 0.0001))
        let NdotV = max(dot(N, V), 0.001)
        let F0 = mix(vec3(0.04, 0.04, 0.04), albedo.rgb, materialMetallic)
        let oneOverPi = 0.31830988618

        // Full Cook-Torrance BRDF per light. keyContrib/f1..f7 already carry
        // radiance * attenuation * NdotL * shadow, so each BRDF is multiplied
        // by its own light instead of reusing slot 0's Fresnel/specular lobe.
        let brdfRawL0 = Ld0 * isDir0 + Lp0 * (isPoint0 + isSpot0)
        let brdfL0 = brdfRawL0 * (1.0 / max(length(brdfRawL0), 0.0001))
        let brdfRawH0 = V + brdfL0
        let brdfH0 = brdfRawH0 * (1.0 / max(length(brdfRawH0), 0.0001))
        let brdfNdotL0 = max(dot(N, brdfL0), 0.0)
        let brdfF0 = fresnelSchlick(max(dot(V, brdfH0), 0.0), F0)
        let brdfD0 = distributionGGX(max(dot(N, brdfH0), 0.0), materialRoughness)
        let brdfG0 = geometrySmith(NdotV, brdfNdotL0, materialRoughness)
        let brdfDiffuse0 = albedo.rgb * (vec3(1.0, 1.0, 1.0) - brdfF0) * (1.0 - materialMetallic) * oneOverPi
        let brdfSpecular0 = brdfF0 * (brdfD0 * brdfG0 / max(4.0 * NdotV * brdfNdotL0, 0.001))
        let direct0 = (brdfDiffuse0 + brdfSpecular0) * keyContrib

        let brdfRawL1 = Ld1 * isDir1 + Lp1 * (isPoint1 + isSpot1)
        let brdfL1 = brdfRawL1 * (1.0 / max(length(brdfRawL1), 0.0001))
        let brdfRawH1 = V + brdfL1
        let brdfH1 = brdfRawH1 * (1.0 / max(length(brdfRawH1), 0.0001))
        let brdfNdotL1 = max(dot(N, brdfL1), 0.0)
        let brdfF1 = fresnelSchlick(max(dot(V, brdfH1), 0.0), F0)
        let brdfD1 = distributionGGX(max(dot(N, brdfH1), 0.0), materialRoughness)
        let brdfG1 = geometrySmith(NdotV, brdfNdotL1, materialRoughness)
        let brdfDiffuse1 = albedo.rgb * (vec3(1.0, 1.0, 1.0) - brdfF1) * (1.0 - materialMetallic) * oneOverPi
        let brdfSpecular1 = brdfF1 * (brdfD1 * brdfG1 / max(4.0 * NdotV * brdfNdotL1, 0.001))
        let direct1 = (brdfDiffuse1 + brdfSpecular1) * f1

        let brdfRawL2 = Ld2 * isDir2 + Lp2 * (isPoint2 + isSpot2)
        let brdfL2 = brdfRawL2 * (1.0 / max(length(brdfRawL2), 0.0001))
        let brdfRawH2 = V + brdfL2
        let brdfH2 = brdfRawH2 * (1.0 / max(length(brdfRawH2), 0.0001))
        let brdfNdotL2 = max(dot(N, brdfL2), 0.0)
        let brdfF2 = fresnelSchlick(max(dot(V, brdfH2), 0.0), F0)
        let brdfD2 = distributionGGX(max(dot(N, brdfH2), 0.0), materialRoughness)
        let brdfG2 = geometrySmith(NdotV, brdfNdotL2, materialRoughness)
        let brdfDiffuse2 = albedo.rgb * (vec3(1.0, 1.0, 1.0) - brdfF2) * (1.0 - materialMetallic) * oneOverPi
        let brdfSpecular2 = brdfF2 * (brdfD2 * brdfG2 / max(4.0 * NdotV * brdfNdotL2, 0.001))
        let direct2 = (brdfDiffuse2 + brdfSpecular2) * f2

        let brdfRawL3 = Ld3 * isDir3 + Lp3 * (isPoint3 + isSpot3)
        let brdfL3 = brdfRawL3 * (1.0 / max(length(brdfRawL3), 0.0001))
        let brdfRawH3 = V + brdfL3
        let brdfH3 = brdfRawH3 * (1.0 / max(length(brdfRawH3), 0.0001))
        let brdfNdotL3 = max(dot(N, brdfL3), 0.0)
        let brdfF3 = fresnelSchlick(max(dot(V, brdfH3), 0.0), F0)
        let brdfD3 = distributionGGX(max(dot(N, brdfH3), 0.0), materialRoughness)
        let brdfG3 = geometrySmith(NdotV, brdfNdotL3, materialRoughness)
        let brdfDiffuse3 = albedo.rgb * (vec3(1.0, 1.0, 1.0) - brdfF3) * (1.0 - materialMetallic) * oneOverPi
        let brdfSpecular3 = brdfF3 * (brdfD3 * brdfG3 / max(4.0 * NdotV * brdfNdotL3, 0.001))
        let direct3 = (brdfDiffuse3 + brdfSpecular3) * f3

        let brdfRawL4 = Ld4 * isDir4 + Lp4 * (isPoint4 + isSpot4)
        let brdfL4 = brdfRawL4 * (1.0 / max(length(brdfRawL4), 0.0001))
        let brdfRawH4 = V + brdfL4
        let brdfH4 = brdfRawH4 * (1.0 / max(length(brdfRawH4), 0.0001))
        let brdfNdotL4 = max(dot(N, brdfL4), 0.0)
        let brdfF4 = fresnelSchlick(max(dot(V, brdfH4), 0.0), F0)
        let brdfD4 = distributionGGX(max(dot(N, brdfH4), 0.0), materialRoughness)
        let brdfG4 = geometrySmith(NdotV, brdfNdotL4, materialRoughness)
        let brdfDiffuse4 = albedo.rgb * (vec3(1.0, 1.0, 1.0) - brdfF4) * (1.0 - materialMetallic) * oneOverPi
        let brdfSpecular4 = brdfF4 * (brdfD4 * brdfG4 / max(4.0 * NdotV * brdfNdotL4, 0.001))
        let direct4 = (brdfDiffuse4 + brdfSpecular4) * f4

        let brdfRawL5 = Ld5 * isDir5 + Lp5 * (isPoint5 + isSpot5)
        let brdfL5 = brdfRawL5 * (1.0 / max(length(brdfRawL5), 0.0001))
        let brdfRawH5 = V + brdfL5
        let brdfH5 = brdfRawH5 * (1.0 / max(length(brdfRawH5), 0.0001))
        let brdfNdotL5 = max(dot(N, brdfL5), 0.0)
        let brdfF5 = fresnelSchlick(max(dot(V, brdfH5), 0.0), F0)
        let brdfD5 = distributionGGX(max(dot(N, brdfH5), 0.0), materialRoughness)
        let brdfG5 = geometrySmith(NdotV, brdfNdotL5, materialRoughness)
        let brdfDiffuse5 = albedo.rgb * (vec3(1.0, 1.0, 1.0) - brdfF5) * (1.0 - materialMetallic) * oneOverPi
        let brdfSpecular5 = brdfF5 * (brdfD5 * brdfG5 / max(4.0 * NdotV * brdfNdotL5, 0.001))
        let direct5 = (brdfDiffuse5 + brdfSpecular5) * f5

        let brdfRawL6 = Ld6 * isDir6 + Lp6 * (isPoint6 + isSpot6)
        let brdfL6 = brdfRawL6 * (1.0 / max(length(brdfRawL6), 0.0001))
        let brdfRawH6 = V + brdfL6
        let brdfH6 = brdfRawH6 * (1.0 / max(length(brdfRawH6), 0.0001))
        let brdfNdotL6 = max(dot(N, brdfL6), 0.0)
        let brdfF6 = fresnelSchlick(max(dot(V, brdfH6), 0.0), F0)
        let brdfD6 = distributionGGX(max(dot(N, brdfH6), 0.0), materialRoughness)
        let brdfG6 = geometrySmith(NdotV, brdfNdotL6, materialRoughness)
        let brdfDiffuse6 = albedo.rgb * (vec3(1.0, 1.0, 1.0) - brdfF6) * (1.0 - materialMetallic) * oneOverPi
        let brdfSpecular6 = brdfF6 * (brdfD6 * brdfG6 / max(4.0 * NdotV * brdfNdotL6, 0.001))
        let direct6 = (brdfDiffuse6 + brdfSpecular6) * f6

        let brdfRawL7 = Ld7 * isDir7 + Lp7 * (isPoint7 + isSpot7)
        let brdfL7 = brdfRawL7 * (1.0 / max(length(brdfRawL7), 0.0001))
        let brdfRawH7 = V + brdfL7
        let brdfH7 = brdfRawH7 * (1.0 / max(length(brdfRawH7), 0.0001))
        let brdfNdotL7 = max(dot(N, brdfL7), 0.0)
        let brdfF7 = fresnelSchlick(max(dot(V, brdfH7), 0.0), F0)
        let brdfD7 = distributionGGX(max(dot(N, brdfH7), 0.0), materialRoughness)
        let brdfG7 = geometrySmith(NdotV, brdfNdotL7, materialRoughness)
        let brdfDiffuse7 = albedo.rgb * (vec3(1.0, 1.0, 1.0) - brdfF7) * (1.0 - materialMetallic) * oneOverPi
        let brdfSpecular7 = brdfF7 * (brdfD7 * brdfG7 / max(4.0 * NdotV * brdfNdotL7, 0.001))
        let direct7 = (brdfDiffuse7 + brdfSpecular7) * f7

        let directLit = direct0 + direct1 + direct2 + direct3 + direct4 + direct5 + direct6 + direct7
        let ambientF = fresnelSchlickRoughness(NdotV, F0, materialRoughness)
        let ambientDiffuseWeight = (vec3(1.0, 1.0, 1.0) - ambientF) * (1.0 - materialMetallic)
        let ambientLit = albedo.rgb * ambientDiffuseWeight * ambient
                       * materialAo * ssaoAmbient
        let pbrLit = ambientLit + directLit + surface.rgb
        let unlit = albedo.rgb + surface.rgb
        let isUnlit = step(0.5, materialModel) * (1.0 - step(1.5, materialModel))
        let lit = mix(pbrLit, unlit, isUnlit)
        // �Skybox0 (2026-07-23) ??backdrop blend: sky only shows
        // where lit is near zero (so geometry keeps its color;
        // sky fills gaps in scene coverage). When
        // `ctx.skyboxPass == nullptr` the gbufferSky sampler stays
        // unbound and `sample(gbufferSky, baseUv)` returns black;
        // `mix(black, lit, 1) == lit` collapses to the pre-�Skybox0
        // dark-frame behavior on a Forward / non-sky host.
        let skyColor = sample(gbufferSky, baseUv).xyz * skyMix.x
        let coverage = step(0.5, surface.a)
        let finalColor = mix(skyColor, lit, coverage)
        return vec4(finalColor, coverage)
    }
}
)";

const char* const kLightingPhoskiaSourceCStr = kLightingPhoskiaSource;

static std::string hardShadowBlock(std::string_view block, uint32_t slot)
{
    const std::string n = std::to_string(slot);
    const std::string samplePrefix = "let _s" + n;
    const std::string centerSample = "let _s" + n + "e ";
    const std::string testPrefix = "let _t" + n;
    const std::string centerTest = "let _t" + n + "e ";
    const std::string softPrefix = "let _soft" + n;
    const std::string shadowPrefix = "let _shadow" + n;

    std::string out;
    size_t begin = 0;
    while (begin < block.size()) {
        const size_t newline = block.find('\n', begin);
        const size_t end = newline == std::string_view::npos
            ? block.size()
            : newline + 1;
        const std::string_view line = block.substr(begin, end - begin);
        const bool nonCenterSample = line.find(samplePrefix) != std::string_view::npos
            && line.find(centerSample) == std::string_view::npos;
        const bool nonCenterTest = line.find(testPrefix) != std::string_view::npos
            && line.find(centerTest) == std::string_view::npos;
        if (nonCenterSample || nonCenterTest
            || line.find(softPrefix) != std::string_view::npos) {
            begin = end;
            continue;
        }
        if (line.find(shadowPrefix) != std::string_view::npos) {
            out += "        let _shadow" + n
                + " = mix(1.0, _t" + n + "e, _inMap" + n + ")\n";
        } else {
            out.append(line.data(), line.size());
        }
        begin = end;
    }
    return out;
}

std::string buildLightingVariantSource(uint32_t shadowCount, bool pcfEnabled)
{
    shadowCount = std::min(shadowCount, ayt::render::kMaxSceneLights);
    std::string source(kLightingPhoskiaSource);
    if (shadowCount == 0) {
        const std::string commonBegin = "        let _world = vec4(worldPos, 1.0)\n";
        const std::string slotZero = "        // slot 0\n";
        const size_t begin = source.find(commonBegin);
        const size_t end = source.find(slotZero, begin);
        if (begin != std::string::npos && end != std::string::npos) {
            source.replace(begin, end - begin,
                           "        // shadow path compile-time inactive\n\n");
        }
    }
    for (int slot = 0;
         slot < static_cast<int>(ayt::render::kMaxSceneLights);
         ++slot) {
        const std::string marker = "        // slot " + std::to_string(slot) + "\n";
        const std::string nextMarker = slot + 1
            < static_cast<int>(ayt::render::kMaxSceneLights)
            ? "        // slot " + std::to_string(slot + 1) + "\n"
            : "        // --- light 0 (key) ---";
        const size_t begin = source.find(marker);
        const size_t end = source.find(nextMarker, begin + marker.size());
        if (begin == std::string::npos || end == std::string::npos) {
            continue;
        }

        if (static_cast<uint32_t>(slot) >= shadowCount) {
            const std::string inactive =
                "        // slot " + std::to_string(slot)
                + " (compile-time inactive)\n"
                + "        let perLightShadow" + std::to_string(slot)
                + " = vec4(1.0)\n\n";
            source.replace(begin, end - begin, inactive);
        } else if (!pcfEnabled) {
            const std::string block = source.substr(begin, end - begin);
            source.replace(begin, end - begin,
                           hardShadowBlock(block, static_cast<uint32_t>(slot)));
        }
    }
    return source;
}

LightingPass::~LightingPass() = default;

void LightingPass::setOutputSize(uint16_t width, uint16_t height) noexcept
{
    // �P5 B5 (2026-07-22) ??host-driven store-only call (mirror
    // GBufferPass::setGbufferSize at GBufferPass.cpp:231-238).
    // No adapter access here; the next execute() honors the size.
    _lightingW = width;
    _lightingH = height;
    _producedThisFrame = false;
}

void LightingPass::prepareOutput(BGFXAdapter& adapter)
{
    ensure(adapter, _lightingW, _lightingH);
}

void LightingPass::destroyResources(BGFXAdapter& adapter)
{
    // �P5 B5 (2026-07-22) ??mirror GBufferPass::destroyResources at
    // GBufferPass.cpp:240-266. Drop the FBO, fullscreen VB/IB, and
    // reset all cached handles. W/H + buildStamp reset UNCONDITIONALLY
    // so a host that calls setOutputSize(800,600) ??destroyResources()
    // expects W/H back to 0 (Test_B5 case 6 pins this). The FBO/VB/IB
    // handles are only destroyed when actually allocated (calling
    // bgfx::destroy on an invalid handle is a UAF on some bgfx
    // backends ??mirror ShadowMapResources::destroy guard).
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
    _programVariantKey.clear();
    _producedThisFrame = false;
    // �P5.5 D (2026-07-23) ??cube binding IDs reset on destroy so
    // the next ensureProgram() re-resolves them after the v22
    // cache-key bump forces a re-acquire.
    _tEnvCube         = ayt::shader::InvalidBinding;
    _uCubeActive      = ayt::shader::InvalidBinding;
    _uAmbientStrength = ayt::shader::InvalidBinding;
    _tSsaoTexture     = ayt::shader::InvalidBinding;
    _uSsaoParams      = ayt::shader::InvalidBinding;
    _uShadowAtlasRects    = ayt::shader::InvalidBinding;
    _uLightViewProjs      = ayt::shader::InvalidBinding;
    _uShadowBiases        = ayt::shader::InvalidBinding;
    _uPerLightShadowCount = ayt::shader::InvalidBinding;
    _uActiveLightCount = ayt::shader::InvalidBinding;
}

void LightingPass::ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    // �P5 B5 (2026-07-22) ??mirror GBufferPass::ensure at
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

    // Rebuild path ??drop old FBO + recreate
    if (bgfx::isValid(_lightingFbo)) {
        adapter.destroy(_lightingFbo);
        _lightingFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _allocatedW = _allocatedH = 0;
    }

    // HDR lighting target. Keep values above 1.0 alive through bloom/haze;
    // PostProcessPass owns the final tone-map/gamma conversion.
    // attachment ??this is a fullscreen post-process pass that
    // does not read depth. `withDepth=false` matches cutsheet
    // `pass-lessons-from-deferred.md:151,161,169` "LightingPass
    // ??LightingOutput FBO" semantics (independent RGBA16F RT, not
    // a color+depth like sceneFbo).
    _lightingFbo = adapter.createFrameBuffer(width, height,
                                              kHdrSceneColorFormat,
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
    // �P5 B5 (2026-07-22) ??mirror PostProcessPass::ensureFullscreenQuad
    // at PostProcessPass.cpp:280-309. Lazy creation; VB/IB cached
    // after first call.
    if (BGFXAdapter::isValid(_fullscreenVB)
        && BGFXAdapter::isValid(_fullscreenIB)) {
        return;
    }
    const bgfx::VertexLayout layout = adapter.vertexLayoutPosUv();
    (void)ensureFullscreenTriangleBuffers(
        adapter, _fullscreenVB, _fullscreenIB,
        kLightingFullscreenTriangle, sizeof(kLightingFullscreenTriangle), layout,
        kLightingFullscreenIndices, sizeof(kLightingFullscreenIndices));
}

void LightingPass::ensureProgram(ayt::shader::ShaderResourcePool& pool,
                                 uint32_t shadowCount,
                                 bool pcfEnabled)
{
    // �P5 B5 (2026-07-22) ??mirror GBufferPass::ensureProgram at
    // GBufferPass.cpp:72-107. Stamp-checked `s_acquiredCacheKey`
    // pointer-equal guard (ShadowCaster Issue 1 fix at
    // ShadowCaster.cpp:60-65). On compile failure: log + set
    // _programAcquireFailed, leave _programReady false. On success:
    // _program = acquired, _programReady = true.
    shadowCount = std::min(shadowCount, ayt::render::kMaxSceneLights);
    const std::string requestedKey = std::string(kLightingCacheKey)
        + "_shadow" + std::to_string(shadowCount)
        + (pcfEnabled ? "_pcf9" : "_hard1");
    if (_programVariantKey != requestedKey) {
        _program.reset();
        _programReady = false;
        _programAcquireFailed = false;
        // �P5.5 D (2026-07-23) ??cube binding IDs reset on cache-
        // key bump (v21 ??v22). Mirror shadowMap / gbufferSky
        // binding reset pattern at the top of ensureProgram.
        _tEnvCube         = ayt::shader::InvalidBinding;
        _uCubeActive      = ayt::shader::InvalidBinding;
        _uAmbientStrength = ayt::shader::InvalidBinding;
        _tSsaoTexture     = ayt::shader::InvalidBinding;
        _uSsaoParams      = ayt::shader::InvalidBinding;
        _uShadowAtlasRects    = ayt::shader::InvalidBinding;
        _uLightViewProjs      = ayt::shader::InvalidBinding;
        _uShadowBiases        = ayt::shader::InvalidBinding;
        _uPerLightShadowCount = ayt::shader::InvalidBinding;
        _uActiveLightCount = ayt::shader::InvalidBinding;
        _programVariantKey = requestedKey;
    }

    if (_program.isValid() || _programAcquireFailed) {
        return;
    }

    const std::string variantSource =
        buildLightingVariantSource(shadowCount, pcfEnabled);
    ayt::shader::ShaderResource acquired =
        pool.acquire(variantSource, _programVariantKey);
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
                  _programVariantKey.c_str());

    _program = acquired;
    _programReady = true;

    // �P5.5 D (2026-07-23) ??lazy-resolve IBL MVP binding IDs.
    // Default InvalidBinding on acquire failure; the FS's
    // `ambientCube * cubeActive` collapses to 0 (ambientFlat only)
    // when cubeActive=0 OR envCube binding is invalid. The
    // cubeActive uniform binding is the per-frame gate that
    // distinguishes pre-D flat ambient vs normal-driven ambient.
    _tEnvCube         = _program.getTextureBinding("envCube");
    _uCubeActive      = _program.getUniformBinding("cubeActive");
    _uAmbientStrength = _program.getUniformBinding("ambientStrength");
    _tSsaoTexture     = _program.getTextureBinding("ssaoTexture");
    _uSsaoParams      = _program.getUniformBinding("ssaoParams");

    // �P5.5 C (2026-07-23) ??per-light shadow atlas binding
    // resolves. Default InvalidBinding on acquire failure; the
    // FS's `mix(vec4(1.0), vec4(_shadow_i), step(i+0.5, N))`
    // collapses perLightShadow{i} = vec4(1.0) when the binding
    // is invalid AND/OR perLightShadowCount.x = 0 ??preserving
    // the pre-C byte-equivalent (key-only shadow multiply).
    _uShadowAtlasRects    = _program.getUniformBinding("shadowAtlasRects");
    _uLightViewProjs      = _program.getUniformBinding("lightViewProjs");
    _uShadowBiases        = _program.getUniformBinding("shadowBiases");
    _uPerLightShadowCount = _program.getUniformBinding("perLightShadowCount");
    _uActiveLightCount    = _program.getUniformBinding("activeLightCount");
}

uint32_t LightingPass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;

    // �P5 B5 (2026-07-22) ??first real GPU work in the Deferred
    // LightingPass. Phoskia VS/FS acquires, binds view 8 to the
    // LightingOutput FBO, dispatches a fullscreen triangle that
    // samples 3 GBuffer attachments + applies Lambert directional
    // light (1 direction from FrameContext::lightDirection, 1 color
    // from FrameContext::lightColor).
    //
    // B5.5 (2026-07-22) ??consumes `ctx.shadowPass->shadowMap()` via
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
    // RT2 is a live input: xyz world position for local lights/shadows and
    // w packed AO/material-model data.
    //
    // �5.4 (2026-07-22, FO/Trans PR) ??`isInitialized()` guard only,
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

    // A valid FBO is not proof that the producer ran this frame. Gate before
    // allocating/dispatching consumers so a failed GBuffer compile, disabled
    // pass, zero viewport, or caught exception cannot feed stale MRT contents
    // into the lighting fullscreen draw.
    BlackboardGBufferView blackboardGBuffer;
    const bool useBlackboard = ctx.resourceBlackboard != nullptr;
    const bool blackboardGBufferReady = useBlackboard
        && ctx.resourceBlackboard->resolveGBuffer(blackboardGBuffer);
    const bool legacyGBufferReady = !useBlackboard
        && ctx.gbufferPass != nullptr
        && ctx.gbufferPass->producedThisFrame()
        && ctx.gbufferPass->hasValidAttachments();
    if (!blackboardGBufferReady && !legacyGBufferReady) {
        return 0;
    }
    const bgfx::TextureHandle gbufferAlbedo = blackboardGBufferReady
        ? blackboardGBuffer.albedo : ctx.gbufferPass->gbufferAlbedoRt();
    const bgfx::TextureHandle gbufferNormal = blackboardGBufferReady
        ? blackboardGBuffer.normal : ctx.gbufferPass->gbufferNormalRt();
    const bgfx::TextureHandle gbufferWorldPosition = blackboardGBufferReady
        ? blackboardGBuffer.worldPosition
        : ctx.gbufferPass->gbufferWorldPositionRt();
    const bgfx::TextureHandle gbufferMaterial = blackboardGBufferReady
        ? blackboardGBuffer.material : ctx.gbufferPass->gbufferMaterialRt();

    ensure(ctx.adapter, _lightingW, _lightingH);
    if (!bgfx::isValid(_lightingFbo)) {
        return 0;
    }

    ensureFullscreenQuad(ctx.adapter);
    if (!BGFXAdapter::isValid(_fullscreenVB)
        || !BGFXAdapter::isValid(_fullscreenIB)) {
        return 0;
    }

    const uint32_t shadowVariantCount =
        ctx.shadowPass != nullptr && ctx.shadowPass->hasSampleableShadow()
        ? std::min(ctx.shadowPass->perLightShadowCount(),
                   ayt::render::kMaxSceneLights)
        : 0u;
    const bool shadowVariantPcf = shadowVariantCount > 0
        && ctx.shadowPass->pcfEnabled();
    ensureProgram(ctx.pool, shadowVariantCount, shadowVariantPcf);
    if (!_program.isValid()
        || _tSsaoTexture == ayt::shader::InvalidBinding
        || _uSsaoParams == ayt::shader::InvalidBinding) {
        return 0;
    }

    const FrameContext& frame = ctx.frame;
    const bool skyReady = ctx.skyboxPass != nullptr
        && ctx.skyboxPass->isEnabled()
        && ctx.skyboxPass->producedThisFrame()
        && bgfx::isValid(ctx.skyboxPass->skyRt());

    // View 8 wiring: bind LightingOutput FBO, set rect to viewport
    // size, clear, dispatch fullscreen triangle. Mirror GBufferPass
    // view 7 wiring at GBufferPass.cpp:157-176 but with
    // LightingOutput FBO + view 8.
    const uint8_t viewId = kLightingViewId;
    ctx.adapter.setViewTransform(viewId, frame.view, frame.projection);
    ctx.adapter.setViewFrameBuffer(viewId, _lightingFbo);
    ctx.adapter.setViewRect(viewId, 0, 0, _lightingW, _lightingH);
    // Clear to black (matches cutsheet �5.2 "GBuffer/Lighting ??
    // clear=0" lock ??black is the correct "no light contribution"
    // baseline; lit fragments overwrite).
    ctx.adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR,
                                /*rgba=*/0x000000ff,
                                /*depth=*/1.0f,
                                /*stencil=*/0);

    // B5 lighting state: WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS
    // (no depth read, no depth write ??fullscreen post-process).
    // BGFXAdapter doesn't expose a setStatePostProcess helper yet,
    // so use the inline form via setStateOpaque() + override: we
    // bypass setStateOpaque and use bgfx::setState directly via
    // adapter.setStateOpaque() as the base (DEPTH_TEST_LESS +
    // back-face culling) — but for a fullscreen post-process pass culling is
    // wrong (the oversize triangle needs no culling). Use
    // setStateDepthTestAlways() (verified at BGFXAdapter.cpp:171-178
    // documentation: "PostProcessPass fullscreen blit"). That's
    // WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS + no face culling.
    ctx.adapter.setStateDepthTestAlways();

    // Production dispatch resolves the coherent MRT set from the resource
    // blackboard. Direct pass tests without a blackboard retain the legacy
    // borrowed-pointer path above.
    {
        // albedo sampler ??stage from compiled Phoskia
        const shader::BindingId albedoBinding =
            _program.getTextureBinding("gbufferAlbedo");
        if (albedoBinding != shader::InvalidBinding) {
            const bgfx::TextureHandle albedoHandle = gbufferAlbedo;
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
            const bgfx::TextureHandle normalHandle = gbufferNormal;
            if (bgfx::isValid(normalHandle)) {
                const uint8_t stage = _program.getTextureStage(normalBinding);
                _program.setTexture(stage, normalBinding,
                                    toShaderTexture(normalHandle));
            }
        }
        const shader::BindingId worldPositionBinding =
            _program.getTextureBinding("gbufferWorldPosition");
        if (worldPositionBinding != shader::InvalidBinding) {
            const bgfx::TextureHandle worldPositionHandle =
                gbufferWorldPosition;
            if (bgfx::isValid(worldPositionHandle)) {
                const uint8_t stage =
                    _program.getTextureStage(worldPositionBinding);
                _program.setTexture(stage, worldPositionBinding,
                                    toShaderTexture(worldPositionHandle));
            }
        }
        const shader::BindingId materialBinding =
            _program.getTextureBinding("gbufferMaterial");
        if (materialBinding != shader::InvalidBinding) {
            const bgfx::TextureHandle materialHandle = gbufferMaterial;
            if (bgfx::isValid(materialHandle)) {
                const uint8_t stage = _program.getTextureStage(materialBinding);
                _program.setTexture(stage, materialBinding,
                                    toShaderTexture(materialHandle));
            }
        }
        // �Skybox0 (2026-07-23) ??gbufferSky backdrop sampler.
        // Only this frame's successful sky submission may feed Lighting.
        // An inactive/failed producer must not expose a retained attachment.
        const shader::BindingId skyBinding =
            _program.getTextureBinding("gbufferSky");
        if (skyBinding != shader::InvalidBinding) {
            // Always bind a defined sampler. The finite RGBA8 fallback is
            // multiplied by zero below when no current-frame sky exists.
            bgfx::TextureHandle skyHandle = gbufferMaterial;
            if (skyReady) {
                skyHandle = ctx.skyboxPass->skyRt();
            }
            if (bgfx::isValid(skyHandle)) {
                const uint8_t stage = _program.getTextureStage(skyBinding);
                _program.setTexture(stage, skyBinding,
                                    toShaderTexture(skyHandle));
                // §P3 M6 / L10 (2026-08-24) — `_gbufferSkyRt` field
                // deleted; the live handle is rebound every frame.
            } else {
                // Sky handle invalid: sampler stays unbound; FS samples
                // a black gbufferSky and the backdrop blend collapses
                // to `lit` (pre-§Skybox0 behavior).
            }
        }
    }

    // SSAO is generated before Lighting and is accepted only when both the
    // producer latch and FrameGraph semantic refer to this frame. A valid
    // persistent attachment alone is never considered sufficient.
    bgfx::TextureHandle ssaoTexture =
        gbufferMaterial; // always-valid fallback sampler
    bool ssaoReady = false;
    if (useBlackboard) {
        const BlackboardResourceEntry* ssao =
            ctx.resourceBlackboard->findProduced(
                BlackboardResourceId::SsaoOcclusion);
        if (ssao != nullptr && BGFXAdapter::isValid(ssao->texture)) {
            ssaoTexture = ssao->texture;
            ssaoReady = true;
        }
    } else if (ctx.ssaoPass != nullptr
               && ctx.ssaoPass->producedThisFrame()
               && ctx.frameGraph != nullptr) {
        const bgfx::FrameBufferHandle ssaoFbo =
            ctx.frameGraph->resolveSemantic(FgSemantic::SSAOSource);
        if (BGFXAdapter::isValid(ssaoFbo)) {
            const bgfx::TextureHandle candidate =
                ctx.adapter.getFboAttachment(ssaoFbo, 0);
            if (BGFXAdapter::isValid(candidate)) {
                ssaoTexture = candidate;
                ssaoReady = true;
            }
        }
    }
    _program.setTexture(_program.getTextureStage(_tSsaoTexture),
                        _tSsaoTexture,
                        toShaderTexture(ssaoTexture));
    const float ssaoParams[4] = {
        ssaoReady ? frame.ssaoStrength : 0.0f,
        1.0f / static_cast<float>(_lightingW),
        1.0f / static_cast<float>(_lightingH),
        0.0f,
    };
    _program.setUniform(_uSsaoParams, ssaoParams, sizeof(ssaoParams));

    // �Skybox0 (2026-07-23) ??upload `skyMix` uniform. Default =
    // ayt::render::kDefaultSkyMix (full intensity; hoisted to
    // RenderTypes.h by â§P3 M1, 2026-08-24). Per-frame override
    // remains the host\u’s responsibility via the uniform binding
    // acquired here; this constant is the in-pass default, not an override. Phoskia
    // Vec4 ABI (bgfx Vec4 slot, see docs/pass-lessons-from-shadow.md
    // �3.1) ??scalar in .x, pad .yzw = 0. Bound unconditionally ??
    // Inactive sky uses a defined fallback sampler and zero skyMix; do not
    // assume an unbound sampler is black on every backend.
    const shader::BindingId skyMixBinding =
        _program.getUniformBinding("skyMix");
    if (skyMixBinding != shader::InvalidBinding) {
        const float skyMixPad[4] = {
            skyReady ? ayt::render::kDefaultSkyMix : 0.0f, 0.0f, 0.0f, 0.0f };
        _program.setUniform(skyMixBinding, skyMixPad, sizeof(skyMixPad));
    }

    // �P5.5 D (2026-07-23) ??IBL MVP cube path: bind envCube
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

    // �P5.5 D (2026-07-23) ??upload cubeActive (0.0 or 1.0) +
    // ambientStrength (default 0.6). cubeActive mirrors the same
    // predicate SkyboxPass::execute uses for skyKind so the two
    // paths can never disagree per frame. ambientStrength is a
    // constant scalar ??the host can override via per-material
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
    // same 16-byte padded upload pattern ??see RenderPass.cpp
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

    // �P5 B5 (2026-07-22) ??upload 3 light uniforms from FrameContext
    // (cutsheet �5.3 NO new FrameContext fields ??reuse
    // lightDirection/lightColor/cameraPosition already shipped).
    //
    // lightDirection: FrameContext stores FROM-light; Phoskia NdotL
    // expects TO-light (same as ForwardOpaquePass / TransparentPass
    // `-frame.lightDirection`). Negate once on upload.
    //
    // Phoskia uniform ABI: vec4 uniforms are uploaded as vec4 (one
    // vec4 = bgfx Vec4 slot ??see docs/pass-lessons-from-shadow.md
    // �3.1). 3-float vec3 ??4-float padded with .w = 0 (or 1 for
    // position-like).
    // §P3 M2 (2026-08-24) - hoisted to
    // RenderPass::trySetUniformVec4. The three blocks below
    // used to be byte-for-byte identical inline
    // `if (binding != Invalid) { float pad[4] = {...};
    // setUniform(...); }` pattern; now a single helper call
    // each. lightDirection negation stays inline (one extra
    // .w=0 pad). Phoskia uniform ABI: vec4 uniforms uploaded
    // as vec4 (one vec4 = bgfx Vec4 slot, see docs/pass-
    // lessons-from-shadow.md §3.1). 3-float vec3 padded
    // with .w = 0 (or 1 for position-like).
    const float lightDir[4] = {
        -frame.lightDirection.x,  // Match ForwardOpaquePass /
        -frame.lightDirection.y,  // TransparentPass: Phoskia
        -frame.lightDirection.z,  // NdotL expects TO-light,
        0.0f,                     // FrameContext stores FROM-light
    };
    const float lightColor[4] = {
        frame.lightColor.x,
        frame.lightColor.y,
        frame.lightColor.z,
        0.0f,
    };
    const float cameraPos[4] = {
        frame.cameraPosition.x,
        frame.cameraPosition.y,
        frame.cameraPosition.z,
        1.0f,  // position-like: .w = 1
    };
    trySetUniformVec4(_program, "u_lightDirection", lightDir);
    trySetUniformVec4(_program, "u_lightColor",      lightColor);
    trySetUniformVec4(_program, "u_cameraPos",       cameraPos);


    // �P5 B7+ (2026-07-22) + �P5.5 A (2026-07-23) + �P5.5 B (2026-07-23)
    // ??multi-light DataSource upload via the host-supplied
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
    // (cutsheet �P5.5 B #3 invariant: default LightType = Directional
    // ??byte-equivalent to pre-B).
    //
    // Per-type CPU pack (cutsheet �P5.5 B):
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
    // `setUniformBlock`) ??one std140-compatible buffer, one binding.
    //
    // Layout (kMaxSceneLights = 8, defined in include/AYRenderer/RenderScene.h):
    //   - bytes [0,   128): dirs[8] * vec4 = 8 * 16 = 128 bytes
    //                       (xyz = light vector, w = float(LightType))
    //   - bytes [128, 256): colors[8] * vec4 = 8 * 16 = 128 bytes
    //   - bytes [256, 384): params[8] * vec4 = 8 * 16 = 128 bytes (B new)
    //                       (x = range, y = intensity, z = coneCosInner,
    //                        w = coneCosOuter)
    //   - bytes [384, 512): spotDir[8] * vec4 = 8 * 16 = 128 bytes (B new)
    //                       (xyz = spotDirection, w = 0)
    // Always upload the full 512-byte layout regardless of
    // lights.count ??Phoskia unroll uses unused slots as zero-vectors.
    //
    // Fallback: ctx.sceneLights == nullptr or count == 0 ??upload
    // one "dirs[0] = -frame.lightDirection, colors[0] = frame.lightColor"
    // pair so the unrolled FS still produces a reasonable image
    // (matches B5 single-light behavior). Params + spotDir slots stay
    // zero ??Directional branch never reads them. This avoids the
    // "FS unrolls 8 lights but UBO has all zero ??black picture"
    // failure mode when the host hasn't called setSceneLights yet.
    const PackedSceneLighting packedLights =
        packSceneLighting(ctx.sceneLights, frame);
    // 4 setUniform calls (one per array), all 128 bytes each.
    // Logged-once message confirms we're on the B field-split path.
    // �P5.5 A ??field name stays `dirs` (xyz = light vector, w = type).
    // Do NOT rename to `record` ??that broke HLSL/bgfx uniform wiring.
    if (!uploadSceneLighting(_program, packedLights)) {
        static uint32_t s_fatalFrame = 0;
        if ((s_fatalFrame++ % 64u) == 0u) {
            std::fprintf(stderr,
                         "[LightingPass] FATAL missing scene-light array contract\n");
        }
    }

    // Lighting and TransparentPass consume the same stable caster-first
    // atlas layout, so light slot i always matches shadow slot i.
    const PackedShadowAtlas packedShadows =
        packShadowAtlas(ctx.shadowPass, true);
    if (!uploadShadowAtlas(_program, packedShadows)) {
        static bool s_loggedMissingShadowContract = false;
        if (!s_loggedMissingShadowContract) {
            s_loggedMissingShadowContract = true;
            std::fprintf(stderr,
                         "[LightingPass] FATAL missing shadow atlas array contract\n");
        }
    }
    // �P5 B5.5 (2026-07-22) ??consume ctx.shadowPass->shadowMap
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
    // nullptr` or `!hasSampleableShadow()` ??both equal `false`
    // here. So the helper always uploads uniforms + binds the
    // sampler on stage 1 (or, on the Noop path, falls back to the
    // adapter's lit-white texture + identity LVP so the FS reads
    // still sample valid data without UB).
    //
    // Same shared-shadow contract for all 8 lights (cutsheet �10
    // pass-lessons-from-deferred.md:300 ??"LightingPass ??
    // ShadowPass ????"): only lights[0] gets the shadow
    // attenuation. Fill / rim lights (1..7) stay unshadowed ??
    // they don't have their own shadow map in v0. Per-light CSM
    // is a separate future cut.
    tryBindShadowSampler(_program, ctx.adapter, ctx.shadowPass,
                         kShadowCastAndReceive, frame.shadowBias);

    // World position comes from RT2, already
    // bound above with the other GBuffer color attachments. No depth
    // reconstruct / u_depthToClip / gbufferDepth bind.

    // �P5 B5 (2026-07-22) ??fullscreen triangle dispatch. B5
    // submits exactly 1 draw (the fullscreen triangle), not N.
    // `_program.submit()` writes viewId + draws using the
    // Adapter-set state. setTransform is a no-op for a fullscreen
    // post-process pass (no model matrix), but the API still wants
    // a valid identity; cheaper to skip ??the FS only reads the
    // viewId and the state.
    ctx.adapter.setTransform(ayt::math::Float4x4::identity());
    ctx.adapter.setVertexBuffer(_fullscreenVB, 0, UINT32_MAX);
    ctx.adapter.setIndexBuffer(_fullscreenIB, 0, 3);

    shader::DrawCallContext submitCtx;
    submitCtx.viewId = viewId;
    submitCtx.state  = 0;  // state owned by Adapter
    _program.submit(submitCtx);

    _producedThisFrame = true;
    return 1;  // B5 ships exactly 1 draw (fullscreen triangle)
}

} // namespace ayt::render::detail
