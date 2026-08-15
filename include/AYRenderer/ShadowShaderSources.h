#pragma once

#include "AYRenderer/ShadowSettings.h"

namespace ayt::render
{

// Phoskia shadow sources — production default.
// Depth encoding matches verified hand .sc: store ndc01 in R (replicate RGB),
// clear=0xffffffff → .r=1 lit; sample().r compare. (RGBA pack deferred —
// pack(≈1)→0 fights the clear/heuristic contract.)
// Fallback hand .sc: AY_SHADOW_USE_SC=1.
inline constexpr const char* kShadowCasterPhoskiaSource = R"(
material ShadowCaster {
    property casterSolidTest = vec4(0.0, 0.0, 0.0, 0.0)
    vertex {
        in pos : position
        out clipZw : texcoord = vec2(
            (modelViewProjection * vec4(pos, 1.0)).z,
            (modelViewProjection * vec4(pos, 1.0)).w)
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in clipZw : texcoord
        let zw = clipZw.x * (1.0 / clipZw.y)
        let ndc01 = zw * 0.5 + 0.5
        let t = step(0.5, casterSolidTest.x)
        let ndOut = ndc01 * (1.0 - t) + 0.5 * t
        return vec4(ndOut, ndOut, ndOut, 1.0)
    }
}
)";

inline constexpr const char* kShadowCasterVaryingSc = R"(
vec2 v_clipZw   : TEXCOORD0 = vec2(0.0, 0.0);
vec3 a_position : POSITION;
)";

inline constexpr const char* kShadowCasterVertexSc = R"(
$input a_position
$output v_clipZw

#include <bgfx_shader.sh>

void main()
{
    vec4 clip = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_clipZw = vec2(clip.z, clip.w);
    gl_Position = clip;
}
)";

inline constexpr const char* kShadowCasterFragmentSc = R"(
$input v_clipZw

#include <bgfx_shader.sh>

uniform vec4 casterSolidTest;

void main()
{
    float zw = v_clipZw.x * (1.0 / v_clipZw.y);
    float ndc01 = zw * 0.5 + 0.5;
    float t = step(0.5, casterSolidTest.x);
    float ndOut = ndc01 * (1.0 - t) + 0.5 * t;
    gl_FragColor = vec4(ndOut, ndOut, ndOut, 1.0);
}
)";

// Lit receiver — ABI matches verified hand .sc (all lighting/bias as vec4,
// swizzle .xyz / .x). Unrolled 3x3 PCF + in-map gate.
inline constexpr const char* kSimpleLitShadowPhoskiaSource = R"(
material SimpleLitShadow {
    texture2d albedoMap
    texture2d shadowMap
    uniform mat4 u_lightViewProj
    uniform vec4 lightDir
    uniform vec4 lightColor
    uniform vec4 shadowMapTexel
    property baseColor      = vec4(1.0, 1.0, 1.0, 1.0)
    property shadowBias     = vec4(0.003, 0.0, 0.0, 0.0)
    property shadowDebugVis = vec4(0.0, 0.0, 0.0, 0.0)
    property shadowPcf      = vec4(1.0, 0.0, 0.0, 0.0)

    vertex {
        in pos : position
        in nrm : normal
        in uv  : texcoord
        out worldNormal : normal   = (modelMatrix * vec4(nrm, 0.0)).xyz
        out uvOut       : texcoord = uv
        out worldPos    : position = (modelMatrix * vec4(pos, 1.0)).xyz
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in worldNormal : normal
        in uvOut       : texcoord
        in worldPos    : position
        let albedo   = sample(albedoMap, uvOut) * baseColor
        let clipPos  = u_lightViewProj * vec4(worldPos, 1.0)
        let invW     = 1.0 / clipPos.w
        let ndcX     = clipPos.x * invW
        let ndcY     = clipPos.y * invW
        let refNdc01 = clipPos.z * invW * 0.5 + 0.5
        let uy       = ndcY * 0.5 + 0.5
        let shadowUv = vec2(ndcX * 0.5 + 0.5, 1.0 - uy)
        let inMap    = step(0.0, shadowUv.x) * step(shadowUv.x, 1.0)
                     * step(0.0, shadowUv.y) * step(shadowUv.y, 1.0)
        let tx = shadowMapTexel.x
        let ty = shadowMapTexel.y
        let bias = shadowBias.x
        let o00 = sample(shadowMap, shadowUv + vec2(-tx, -ty)).x
        let o10 = sample(shadowMap, shadowUv + vec2(0.0, -ty)).x
        let o20 = sample(shadowMap, shadowUv + vec2(tx, -ty)).x
        let o01 = sample(shadowMap, shadowUv + vec2(-tx, 0.0)).x
        let o11 = sample(shadowMap, shadowUv + vec2(0.0, 0.0)).x
        let o21 = sample(shadowMap, shadowUv + vec2(tx, 0.0)).x
        let o02 = sample(shadowMap, shadowUv + vec2(-tx, ty)).x
        let o12 = sample(shadowMap, shadowUv + vec2(0.0, ty)).x
        let o22 = sample(shadowMap, shadowUv + vec2(tx, ty)).x
        let s00 = max(1.0 - step(o00 + bias, refNdc01), step(0.999, o00))
        let s10 = max(1.0 - step(o10 + bias, refNdc01), step(0.999, o10))
        let s20 = max(1.0 - step(o20 + bias, refNdc01), step(0.999, o20))
        let s01 = max(1.0 - step(o01 + bias, refNdc01), step(0.999, o01))
        let s11 = max(1.0 - step(o11 + bias, refNdc01), step(0.999, o11))
        let s21 = max(1.0 - step(o21 + bias, refNdc01), step(0.999, o21))
        let s02 = max(1.0 - step(o02 + bias, refNdc01), step(0.999, o02))
        let s12 = max(1.0 - step(o12 + bias, refNdc01), step(0.999, o12))
        let s22 = max(1.0 - step(o22 + bias, refNdc01), step(0.999, o22))
        let shadowSoft = (s00 + s10 + s20 + s01 + s11 + s21 + s02 + s12 + s22) * (1.0 / 9.0)
        let shadowHard = s11
        let shadowFilt = mix(shadowHard, shadowSoft, shadowPcf.x)
        let shadow = mix(1.0, shadowFilt, inMap)
        let ndotl    = max(dot(normalize(worldNormal), normalize(lightDir.xyz)), 0.25)
        let litColor = albedo.rgb * lightColor.xyz * (ndotl * shadow * 0.65 + 0.20)
        let debugRgb = vec3(o11, o11, o11)
        let lit      = mix(litColor, debugRgb, shadowDebugVis.x)
        return vec4(lit, albedo.a)
    }
}
)";

// Hand-authored bgfx .sc — fallback via AY_SHADOW_USE_SC=1 (same R8 contract).
inline constexpr const char* kSimpleLitShadowVaryingSc = R"(
vec3 v_normal    : NORMAL    = vec3(0.0, 0.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 v_worldpos  : TEXCOORD1 = vec3(0.0, 0.0, 0.0);
vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec2 a_texcoord0 : TEXCOORD0;
)";

inline constexpr const char* kSimpleLitShadowVertexSc = R"(
$input a_position, a_normal, a_texcoord0
$output v_normal, v_texcoord0, v_worldpos

#include <bgfx_shader.sh>

void main()
{
    v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;
    v_texcoord0 = a_texcoord0;
    v_worldpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
}
)";

inline constexpr const char* kSimpleLitShadowFragmentSc = R"(
$input v_normal, v_texcoord0, v_worldpos

#include <bgfx_shader.sh>

uniform vec4 lightDir;
uniform vec4 lightColor;
uniform vec4 baseColor;
uniform mat4 u_lightViewProj;
uniform vec4 shadowBias;
uniform vec4 shadowDebugVis;
uniform vec4 shadowMapTexel;
uniform vec4 shadowPcf;

SAMPLER2D(albedoMap, 0);
SAMPLER2D(shadowMap, 1);

float shadowTap(vec2 uv, float refNdc01, float bias)
{
    float occluder = texture2D(shadowMap, uv).x;
    float cleared = step(0.999, occluder);
    float inShadow = step(occluder + bias, refNdc01);
    return max(1.0 - inShadow, cleared);
}

void main()
{
    vec4 albedo = texture2D(albedoMap, v_texcoord0) * baseColor;
    vec3 n = normalize(v_normal);
    vec3 l = normalize(lightDir.xyz);
    float ndotl = max(dot(n, l), 0.25);

    vec4 clipPos = mul(u_lightViewProj, vec4(v_worldpos, 1.0));
    float invW = 1.0 / clipPos.w;
    float ndcX = clipPos.x * invW;
    float ndcY = clipPos.y * invW;
    float refNdc01 = clipPos.z * invW * 0.5 + 0.5;
    float uy = ndcY * 0.5 + 0.5;
    vec2 shadowUv = vec2(ndcX * 0.5 + 0.5, 1.0 - uy);

    float inMap = step(0.0, shadowUv.x) * step(shadowUv.x, 1.0)
                * step(0.0, shadowUv.y) * step(shadowUv.y, 1.0);

    vec2 texel = shadowMapTexel.xy;
    float bias = shadowBias.x;
    float shadowHard = shadowTap(shadowUv, refNdc01, bias);
    float shadowSoft = 0.0;
    shadowSoft += shadowTap(shadowUv + texel * vec2(-1.0, -1.0), refNdc01, bias);
    shadowSoft += shadowTap(shadowUv + texel * vec2( 0.0, -1.0), refNdc01, bias);
    shadowSoft += shadowTap(shadowUv + texel * vec2( 1.0, -1.0), refNdc01, bias);
    shadowSoft += shadowTap(shadowUv + texel * vec2(-1.0,  0.0), refNdc01, bias);
    shadowSoft += shadowHard;
    shadowSoft += shadowTap(shadowUv + texel * vec2( 1.0,  0.0), refNdc01, bias);
    shadowSoft += shadowTap(shadowUv + texel * vec2(-1.0,  1.0), refNdc01, bias);
    shadowSoft += shadowTap(shadowUv + texel * vec2( 0.0,  1.0), refNdc01, bias);
    shadowSoft += shadowTap(shadowUv + texel * vec2( 1.0,  1.0), refNdc01, bias);
    shadowSoft *= (1.0 / 9.0);
    float shadow = mix(shadowHard, shadowSoft, shadowPcf.x);
    shadow = mix(1.0, shadow, inMap);

    vec3 litColor = albedo.xyz * lightColor.xyz * (ndotl * shadow * 0.65 + 0.20);
    vec3 debugRgb = vec3_splat(texture2D(shadowMap, shadowUv).x);
    vec3 lit = mix(litColor, debugRgb, shadowDebugVis.x);
    gl_FragColor = vec4(lit, albedo.w);
}
)";

inline constexpr const char* kSimpleLitShadowScCacheKey = "simple_lit_shadow_bgfx_sc_v8_vec4_abi";

} // namespace ayt::render
