#include "detail/GBufferPass.h"

#include "AYRenderer/RenderTypes.h"
#include "detail/BgfxMatrix.h"
#include "detail/Draw2D.h"
#include "detail/FrameDrawLists.h"
#include "detail/FrameContext.h"
#include "detail/GBufferLayout.h"
#include "detail/RasterConvention.h"
#include "detail/RenderPass.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ayt::render::detail
{

// §P5 B4a (2026-07-22) — build stamp literal (mirror
// ShadowMapResources.h:55). Pointer-equal comparison; callers MUST
// pass this exact literal. Bumping requires a string change here AND
// a re-ensure (next execute() will rebuild the FBO).
// Stamp bump forces GBuffer FBO rebuild after RT2 format changes
// (RGBA8 motion → RGBA16F worldPos). Pointer-equal compare in ensure().
static constexpr const char* kGBufferBuildStamp =
    "material-contract-v3-model-coverage-split";

// §P5 B5.5 / deferred-shadow contract (2026-07-23):
//   RT0 albedo+metallic RGBA8 / RT1 normal+roughness RGBA8 /
//   RT2 worldPos+(AO+material-model) RGBA16F /
//   RT3 emissive+coverage RGBA8 / depth D24S8.
//   Motion NDC encoding is deferred until a dedicated RT exists —
//   writing motion into RGBA8 RT2 previously broke shadows (Lighting
//   projected [0,1] junk as worldPos).
//
// `u_prevViewProj` stays declared + uploaded so B7+ TAA can reclaim
// the slot without another host wire; FS does not write motion today.
//
// Four `out color` slots map to gl_FragData[0..3]:
//   gl_FragData[0] = gbufferAlbedo
//   gl_FragData[1] = gbufferNormal
//   gl_FragData[2] = gbufferWorldPosition
//   gl_FragData[3] = gbufferMaterial
constexpr const char* kGBufferPhoskiaSource = R"(
uniformblock Skeleton {
    mat4 bones[128]
}
material GBufferFill {
    texture2d albedoMap
    texture2d normalMap
    texture2d metallicMap
    texture2d roughnessMap
    texture2d aoMap
    texture2d emissiveMap
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)
    property metallic = vec4(0.0, 0.0, 0.0, 0.0)
    property roughness = vec4(0.5, 0.0, 0.0, 0.0)
    property ao = vec4(1.0, 0.0, 0.0, 0.0)
    property emissive = vec4(0.0, 0.0, 0.0, 0.0)
    property materialModel = vec4(0.0, 0.0, 0.0, 0.0)
    property doubleSided = vec4(0.0, 0.0, 0.0, 0.0)
    property normalYSign = vec4(1.0, 0.0, 0.0, 0.0)
    property castSkinned = vec4(0.0, 0.0, 0.0, 0.0)
    uniform mat4 u_prevViewProj
    uniform mat4 u_normalMatrix
    uniform vec4 cameraPos

    vertex {
        in pos : position
        in nrm : normal
        in tan : tangent
        in uv  : texcoord
        in boneId : boneindices
        in boneWt : boneweights
        out worldNormal : normal = (u_normalMatrix * mix(
            vec4(nrm, 0.0),
            skinningMatrix(boneId, boneWt, Skeleton.bones, vec4(nrm, 0.0)),
            castSkinned.x)).xyz
        out worldTangent : tangent = vec4((modelMatrix * mix(
            vec4(tan.xyz, 0.0),
            skinningMatrix(boneId, boneWt, Skeleton.bones, vec4(tan.xyz, 0.0)),
            castSkinned.x)).xyz, tan.w)
        out worldPos : position = (modelMatrix * mix(
            vec4(pos, 1.0),
            skinningMatrix(boneId, boneWt, Skeleton.bones, vec4(pos, 1.0)),
            castSkinned.x)).xyz
        out vUv         : texcoord = uv
        return modelViewProjection * mix(
            vec4(pos, 1.0),
            skinningMatrix(boneId, boneWt, Skeleton.bones, vec4(pos, 1.0)),
            castSkinned.x)
    }
    fragment {
        in worldNormal : normal
        in worldTangent : tangent
        in worldPos    : position
        in vUv         : texcoord
        out gbufferAlbedo : color = vec4(0.0, 0.0, 0.0, 0.0)
        out gbufferNormal : color = vec4(0.0, 0.0, 0.0, 0.0)
        out gbufferWorldPosition : color = vec4(0.0, 0.0, 0.0, 0.0)
        out gbufferMaterial : color = vec4(0.0, 0.0, 0.0, 0.0)
        let rawN = normalize(worldNormal)
        let tangentSeed = worldTangent.xyz - rawN * dot(worldTangent.xyz, rawN)
                         + vec3(0.000001, 0.0, 0.0)
        let T = normalize(tangentSeed)
        let B = normalize(cross(rawN, T)) * worldTangent.w
        let tangentNormal = sample(normalMap, vUv).xyz * 2.0
                          - vec3(1.0, 1.0, 1.0)
        let mappedN = normalize(T * tangentNormal.x
                              + B * tangentNormal.y * normalYSign.x
                              + rawN * tangentNormal.z)
        let viewDir = normalize(cameraPos.xyz - worldPos)
        let faceSign = mix(1.0, step(0.0, dot(mappedN, viewDir)) * 2.0 - 1.0,
                           max(0.0, min(1.0, doubleSided.x)))
        let n = mappedN * faceSign
        let albedo = sample(albedoMap, vUv) * baseColor
        let materialMetallic = max(0.0, min(1.0, metallic.x * sample(metallicMap, vUv).x))
        let materialRoughness = max(0.045, min(1.0, roughness.x * sample(roughnessMap, vUv).x))
        let materialAo = max(0.0, min(1.0, ao.x * sample(aoMap, vUv).x))
        let packedAoModel = materialModel.x * 2.0 + materialAo
        let materialEmissive = emissive.xyz * sample(emissiveMap, vUv).rgb
        gbufferAlbedo = vec4(albedo.rgb, materialMetallic)
        gbufferNormal = vec4(n * 0.5 + vec3(0.5, 0.5, 0.5), materialRoughness)
        gbufferWorldPosition = vec4(worldPos, packedAoModel)
        gbufferMaterial = vec4(materialEmissive, 1.0)
    }
}
)";

// Cache key: worldPos in RT2 (RGBA16F FBO) for deferred shadow PCF.
static constexpr const char* kGBufferCacheKey =
    "gbuffer_fill_v16_model_coverage_split";
static constexpr const char* kWorldLit2DGBufferCacheKey =
    "gbuffer_world_lit_2d_cutout_v2_tilemap_uv";
static constexpr const char* kGBufferAlphaCutoutCacheKey =
    "gbuffer_fill_v16_alpha_model_coverage_split";

// WorldLit2D is a geometry substage of the existing GBuffer producer. The
// shared unit quad only carries position+UV, so this program derives its
// object-space +Z normal and +X tangent instead of imposing the full 3D mesh
// vertex contract. The resulting MRT encoding is identical to GBufferFill.
constexpr const char* kWorldLit2DGBufferPhoskiaSource = R"(
material WorldLit2DGBufferFill {
    texture2d albedoMap
    texture2d normalMap
    texture2d roughnessMap
    texture2d emissiveMap
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)
    property tint = vec4(1.0, 1.0, 1.0, 1.0)
    property metallic = vec4(0.0, 0.0, 0.0, 0.0)
    property roughness = vec4(0.75, 0.0, 0.0, 0.0)
    property ao = vec4(1.0, 0.0, 0.0, 0.0)
    property emissive = vec4(0.0, 0.0, 0.0, 0.0)
    property materialModel = vec4(0.0, 0.0, 0.0, 0.0)
    property doubleSided = vec4(1.0, 0.0, 0.0, 0.0)
    property normalYSign = vec4(1.0, 0.0, 0.0, 0.0)
    property srcRect = vec4(0.0, 0.0, 1.0, 1.0)
    property flip = vec4(0.0, 0.0, 0.0, 0.0)
    property uvMapping = vec4(0.0, 0.0, 0.0, 0.0)
    property samplingQuality = vec4(1.0, 0.0, 0.0, 0.0)
    uniform vec4 atlasTexel
    property alphaCutoff = vec4(0.0, 0.0, 0.0, 0.0)
    uniform mat4 u_normalMatrix
    uniform vec4 cameraPos

    vertex {
        in pos : position
        in uv : texcoord
        out worldNormal : normal = (u_normalMatrix * vec4(0.0, 0.0, 1.0, 0.0)).xyz
        out worldTangent : tangent = vec4(
            (modelMatrix * vec4(1.0, 0.0, 0.0, 0.0)).xyz, 1.0)
        out worldPos : position = (modelMatrix * vec4(pos, 1.0)).xyz
        out vUv : texcoord = uv
        let sourceU = mix(uv.x, 1.0 - uv.x, flip.x)
        let sourceVBase = 1.0 - uv.y
        let sourceV = mix(sourceVBase, 1.0 - sourceVBase, flip.y)
        let sourceRectUv = vec2(
            mix(srcRect.x, srcRect.z, sourceU),
            mix(srcRect.y, srcRect.w, sourceV))
        vUv = mix(sourceRectUv, uv,
                  max(0.0, min(1.0, uvMapping.x)))
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in worldNormal : normal
        in worldTangent : tangent
        in worldPos : position
        in vUv : texcoord
        out gbufferAlbedo : color = vec4(0.0, 0.0, 0.0, 0.0)
        out gbufferNormal : color = vec4(0.0, 0.0, 0.0, 0.0)
        out gbufferWorldPosition : color = vec4(0.0, 0.0, 0.0, 0.0)
        out gbufferMaterial : color = vec4(0.0, 0.0, 0.0, 0.0)
        let nearestUv = vec2(
            (floor(vUv.x / max(atlasTexel.x, 0.000001)) + 0.5)
                * atlasTexel.x,
            (floor(vUv.y / max(atlasTexel.y, 0.000001)) + 0.5)
                * atlasTexel.y)
        let sampleUv = mix(nearestUv, vUv,
                           step(0.5, samplingQuality.x))
        let albedo = sample(albedoMap, sampleUv) * baseColor * tint
        if (samplingQuality.x > 1.5) {
            let d4 = atlasTexel.xy * 0.25
            albedo = (sample(albedoMap, vUv + vec2(-d4.x, -d4.y))
                    + sample(albedoMap, vUv + vec2( d4.x, -d4.y))
                    + sample(albedoMap, vUv + vec2(-d4.x,  d4.y))
                    + sample(albedoMap, vUv + vec2( d4.x,  d4.y)))
                    * 0.25 * baseColor * tint
        }
        if (samplingQuality.x > 2.5) {
            let d9 = atlasTexel.xy * 0.3333333
            albedo = (sample(albedoMap, vUv + vec2(-d9.x, -d9.y))
                    + sample(albedoMap, vUv + vec2( 0.0,  -d9.y))
                    + sample(albedoMap, vUv + vec2( d9.x, -d9.y))
                    + sample(albedoMap, vUv + vec2(-d9.x,  0.0))
                    + sample(albedoMap, vUv)
                    + sample(albedoMap, vUv + vec2( d9.x,  0.0))
                    + sample(albedoMap, vUv + vec2(-d9.x,  d9.y))
                    + sample(albedoMap, vUv + vec2( 0.0,   d9.y))
                    + sample(albedoMap, vUv + vec2( d9.x,  d9.y)))
                    * 0.1111111 * baseColor * tint
        }
        if (albedo.a < alphaCutoff.x) { discard }
        let rawN = normalize(worldNormal)
        let tangentSeed = worldTangent.xyz - rawN * dot(worldTangent.xyz, rawN)
                         + vec3(0.000001, 0.0, 0.0)
        let T = normalize(tangentSeed)
        let B = normalize(cross(rawN, T)) * worldTangent.w
        let tangentNormal = sample(normalMap, sampleUv).xyz * 2.0
                          - vec3(1.0, 1.0, 1.0)
        let tangentXSign = mix(1.0, -1.0, flip.x)
        let tangentYSign = normalYSign.x * mix(1.0, -1.0, flip.y)
        let mappedN = normalize(T * tangentNormal.x * tangentXSign
                              + B * tangentNormal.y * tangentYSign
                              + rawN * tangentNormal.z)
        let viewDir = normalize(cameraPos.xyz - worldPos)
        let faceSign = mix(1.0, step(0.0, dot(mappedN, viewDir)) * 2.0 - 1.0,
                           max(0.0, min(1.0, doubleSided.x)))
        let n = mappedN * faceSign
        let materialMetallic = max(0.0, min(1.0, metallic.x))
        let materialRoughness = max(0.045, min(1.0,
            roughness.x * sample(roughnessMap, sampleUv).x))
        let materialAo = max(0.0, min(1.0, ao.x))
        let packedAoModel = materialModel.x * 2.0 + materialAo
        let materialEmissive = emissive.xyz * sample(emissiveMap, sampleUv).rgb
        gbufferAlbedo = vec4(albedo.rgb, materialMetallic)
        gbufferNormal = vec4(n * 0.5 + vec3(0.5, 0.5, 0.5), materialRoughness)
        gbufferWorldPosition = vec4(worldPos, packedAoModel)
        gbufferMaterial = vec4(materialEmissive, 1.0)
    }
}
)";

constexpr const char* kGBufferAlphaCutoutVaryingSc = R"(
vec3 v_normal    : NORMAL    = vec3(0.0, 0.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 v_position  : TEXCOORD1 = vec3(0.0, 0.0, 0.0);
vec4 v_tangent   : TEXCOORD2 = vec4(1.0, 0.0, 0.0, 1.0);
vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_tangent   : TANGENT;
vec4 a_indices   : BLENDINDICES;
vec4 a_weight    : BLENDWEIGHT;
)";

constexpr const char* kGBufferAlphaCutoutVertexSc = R"(
$input a_position, a_normal, a_texcoord0, a_tangent, a_indices, a_weight
$output v_normal, v_texcoord0, v_position, v_tangent
#include <bgfx_shader.sh>
uniform mat4 bones[128];
uniform mat4 u_normalMatrix;
uniform vec4 castSkinned;
void main()
{
    vec4 bindPos = vec4(a_position, 1.0);
    vec4 bindNormal = vec4(a_normal, 0.0);
    vec4 bindTangent = vec4(a_tangent.xyz, 0.0);
    vec4 skinPos = a_weight.x * mul(bones[int(a_indices.x)], bindPos)
                 + a_weight.y * mul(bones[int(a_indices.y)], bindPos)
                 + a_weight.z * mul(bones[int(a_indices.z)], bindPos)
                 + a_weight.w * mul(bones[int(a_indices.w)], bindPos);
    vec4 skinNormal = a_weight.x * mul(bones[int(a_indices.x)], bindNormal)
                    + a_weight.y * mul(bones[int(a_indices.y)], bindNormal)
                    + a_weight.z * mul(bones[int(a_indices.z)], bindNormal)
                    + a_weight.w * mul(bones[int(a_indices.w)], bindNormal);
    vec4 skinTangent = a_weight.x * mul(bones[int(a_indices.x)], bindTangent)
                     + a_weight.y * mul(bones[int(a_indices.y)], bindTangent)
                     + a_weight.z * mul(bones[int(a_indices.z)], bindTangent)
                     + a_weight.w * mul(bones[int(a_indices.w)], bindTangent);
    vec4 localPos = mix(bindPos, skinPos, castSkinned.x);
    vec4 localNormal = mix(bindNormal, skinNormal, castSkinned.x);
    vec4 localTangent = mix(bindTangent, skinTangent, castSkinned.x);
    v_normal = mul(u_normalMatrix, localNormal).xyz;
    v_texcoord0 = a_texcoord0;
    v_position = mul(u_model[0], localPos).xyz;
    v_tangent = vec4(mul(u_model[0], localTangent).xyz, a_tangent.w);
    gl_Position = mul(u_modelViewProj, localPos);
}
)";

constexpr const char* kGBufferAlphaCutoutFragmentSc = R"(
$input v_normal, v_texcoord0, v_position, v_tangent
#include <bgfx_shader.sh>
uniform vec4 baseColor;
uniform vec4 alphaCutoff;
uniform vec4 opacity;
uniform vec4 opacitySource;
uniform vec4 cameraPos;
uniform vec4 doubleSided;
uniform vec4 normalYSign;
uniform vec4 metallic;
uniform vec4 roughness;
uniform vec4 ao;
uniform vec4 emissive;
uniform vec4 materialModel;
SAMPLER2D(albedoMap, 0);
SAMPLER2D(opacityMap, 1);
SAMPLER2D(normalMap, 2);
SAMPLER2D(metallicMap, 3);
SAMPLER2D(roughnessMap, 4);
SAMPLER2D(aoMap, 5);
SAMPLER2D(emissiveMap, 6);
void main()
{
    vec4 albedo = texture2D(albedoMap, v_texcoord0) * baseColor;
    vec4 opacitySample = texture2D(opacityMap, v_texcoord0);
    float dedicatedOpacity = mix(opacitySample.r, opacitySample.a,
                                 step(1.5, opacitySource.x));
    float sampledOpacity = mix(1.0, dedicatedOpacity,
                               step(0.5, opacitySource.x));
    float surfaceAlpha = albedo.a * sampledOpacity * clamp(opacity.x, 0.0, 1.0);
    if (surfaceAlpha < alphaCutoff.x) {
        discard;
    }
    vec3 rawN = normalize(v_normal);
    vec3 tangentSeed = v_tangent.xyz - rawN * dot(v_tangent.xyz, rawN)
                     + vec3(0.000001, 0.0, 0.0);
    vec3 T = normalize(tangentSeed);
    vec3 B = normalize(cross(rawN, T)) * v_tangent.w;
    vec3 tangentNormal = texture2D(normalMap, v_texcoord0).xyz * 2.0
                       - vec3(1.0, 1.0, 1.0);
    vec3 mappedN = normalize(T * tangentNormal.x
                           + B * tangentNormal.y * normalYSign.x
                           + rawN * tangentNormal.z);
    vec3 viewDir = normalize(cameraPos.xyz - v_position);
    float faceSign = mix(1.0, step(0.0, dot(mappedN, viewDir)) * 2.0 - 1.0,
                         clamp(doubleSided.x, 0.0, 1.0));
    vec3 n = mappedN * faceSign;
    float materialMetallic = clamp(metallic.x * texture2D(metallicMap, v_texcoord0).x, 0.0, 1.0);
    float materialRoughness = clamp(roughness.x * texture2D(roughnessMap, v_texcoord0).x, 0.045, 1.0);
    float materialAo = clamp(ao.x * texture2D(aoMap, v_texcoord0).x, 0.0, 1.0);
    float packedAoModel = materialModel.x * 2.0 + materialAo;
    vec3 materialEmissive = emissive.xyz * texture2D(emissiveMap, v_texcoord0).rgb;
    gl_FragData[0] = vec4(albedo.rgb, materialMetallic);
    gl_FragData[1] = vec4(n * 0.5 + vec3(0.5, 0.5, 0.5), materialRoughness);
    gl_FragData[2] = vec4(v_position, packedAoModel);
    gl_FragData[3] = vec4(materialEmissive, 1.0);
}
)";

const char* const kGBufferCacheKeyCStr = kGBufferCacheKey;
const char* const kGBufferBuildStampCStr = kGBufferBuildStamp;
const char* const kGBufferPhoskiaSourceCStr = kGBufferPhoskiaSource;
const char* const kWorldLit2DGBufferPhoskiaSourceCStr =
    kWorldLit2DGBufferPhoskiaSource;

GBufferPass::~GBufferPass() = default;

void GBufferPass::ensureProgram(ayt::shader::ShaderResourcePool& pool)
{
    // Mirror ShadowCaster::ensureProgram (ShadowCaster.cpp:48-101).
    // Issue 1 fix (2026-07-21) — `const char*` (constexpr pointer)
    // not std::string. Pointer-equal compare; bumping the literal
    // forces re-acquire.
    static const char* s_acquiredCacheKey = nullptr;
    static const char* s_acquiredWorldLit2DCacheKey = nullptr;
    if (s_acquiredCacheKey != kGBufferCacheKey) {
        _program.reset();
        _alphaCutoutProgram.reset();
        _worldLit2DProgram.reset();
        _acquireFailed = false;
        _alphaCutoutAcquireFailed = false;
        _worldLit2DAcquireFailed = false;
        s_acquiredCacheKey = kGBufferCacheKey;
        s_acquiredWorldLit2DCacheKey = kWorldLit2DGBufferCacheKey;
    } else if (s_acquiredWorldLit2DCacheKey
               != kWorldLit2DGBufferCacheKey) {
        _worldLit2DProgram.reset();
        _worldLit2DAcquireFailed = false;
        s_acquiredWorldLit2DCacheKey = kWorldLit2DGBufferCacheKey;
    }

    if (!_program.isValid() && !_acquireFailed) {
        ayt::shader::ShaderResource acquired =
            pool.acquire(kGBufferPhoskiaSource, kGBufferCacheKey);
        if (!acquired.isValid()) {
            _acquireFailed = true;
            std::fprintf(stderr,
                         "[GBufferPass] acquire failed; GBuffer fill pass will "
                         "run as no-op (no GPU draw). Errors:\n");
            for (const std::string& err : pool.lastCompileErrors()) {
                std::fprintf(stderr, "[GBufferPass]   %s\n", err.c_str());
            }
        } else {
            std::fprintf(stderr,
                         "[GBufferPass] program ready via Phoskia (cacheKey=%s)\n",
                         kGBufferCacheKey);
            _program = acquired;
        }
    }

    // The cutout program is only a sibling of a working regular GBuffer
    // program. Avoid a second compiler/driver attempt when the primary
    // program failed (notably in Noop tests without a configured shaderc).
    if (!_program.isValid()) {
        return;
    }

    if (!_alphaCutoutProgram.isValid() && !_alphaCutoutAcquireFailed) {
        ayt::shader::ShaderResource acquired = pool.acquireFromBgfxSc(
            kGBufferAlphaCutoutVertexSc,
            kGBufferAlphaCutoutFragmentSc,
            kGBufferAlphaCutoutVaryingSc,
            kGBufferAlphaCutoutCacheKey);
        if (!acquired.isValid()) {
            _alphaCutoutAcquireFailed = true;
            std::fprintf(stderr,
                         "[GBufferPass] alpha-cutout program acquire failed; "
                         "alpha-cutout draws will be skipped. Errors:\n");
            for (const std::string& err : pool.lastCompileErrors()) {
                std::fprintf(stderr, "[GBufferPass]   %s\n", err.c_str());
            }
        } else {
            std::fprintf(stderr,
                         "[GBufferPass] alpha-cutout program ready (cacheKey=%s)\n",
                         kGBufferAlphaCutoutCacheKey);
            _alphaCutoutProgram = acquired;
        }
    }

    if (!_worldLit2DProgram.isValid() && !_worldLit2DAcquireFailed) {
        ayt::shader::ShaderResource acquired = pool.acquire(
            kWorldLit2DGBufferPhoskiaSource, kWorldLit2DGBufferCacheKey);
        if (!acquired.isValid()) {
            _worldLit2DAcquireFailed = true;
            std::fprintf(stderr,
                         "[GBufferPass] WorldLit2D program acquire failed; "
                         "WorldLit2D draws will be skipped. Errors:\n");
            for (const std::string& err : pool.lastCompileErrors()) {
                std::fprintf(stderr, "[GBufferPass]   %s\n", err.c_str());
            }
        } else {
            _worldLit2DProgram = acquired;
        }
    }
}

bool GBufferPass::isProgramReady() const noexcept
{
    return _program.isValid();
}

void GBufferPass::setPrevViewProj(const ayt::math::Float4x4& view,
                                  const ayt::math::Float4x4& projection) noexcept
{
    // §P5 B4c (2026-07-22) — host pushes prev view/projection. Stored
    // as-is; execute() builds prevViewProj = proj * view per
    // pass-lessons-from-shadow.md §3.1 P×V ordering.
    //
    // Idempotent: Renderer::render() calls this every frame inside
    // the GBuffer slot block. Re-pushing the same matrices is free
    // (4×4 copy = 64 bytes, negligible).
    //
    // No GPU work here. The actual upload happens inside execute()
    // once the program is ready + we have a valid scene to draw.
    _prevView       = view;
    _prevProjection = projection;
}

uint32_t GBufferPass::execute(PassExecContext& ctx)
{
    _producedThisFrame = false;

    // §P5 B4a (2026-07-22) — ensure 4-attach MRT FBO + cache attachments.
    // §P5 B4b (2026-07-22) — first real GPU draw dispatch in deferred path:
    //   - Phoskia GBuffer VS/FS acquires via pool.acquire
    //   - bind view 7 to the 4-attach MRT FBO
    //   - clear color + depth (must clear color because each attachment
    //     is an offscreen RT, backbuffer clears don't touch them)
    //   - iterate RenderScene.items() and submit draw calls (mirror
    //     ForwardOpaquePass shape, but view 7 + 3-output FS)
    //
    // Lighting consumes all four color attachments; depth remains available
    // to debug and future screen-space passes.
    if (!ctx.adapter.isInitialized() || ctx.adapter.isNoopBackend()) {
        return 0;
    }

    // Disable signal: host called setGbufferSize(0, 0) (or never
    // called setGbufferSize). Mirror ShadowMapResources::ensure
    // early-return on size == 0.
    if (_gbufferW == 0 || _gbufferH == 0) {
        return 0;
    }

    ensure(ctx.adapter, _gbufferW, _gbufferH);
    if (!isReady()) {
        return 0;
    }

    // B4b: Phoskia VS/FS must succeed before we touch the GPU.
    // ensureProgram records _acquireFailed on compile error so we
    // skip dispatch silently (cutsheet §1.7 "no FBO/work" signal).
    ensureProgram(ctx.pool);
    if (!_program.isValid()) {
        return 0;
    }

    const FrameContext& frame = ctx.frame;

    // View 7 wiring: bind MRT FBO, set rect to GBuffer size (full
    // viewport), clear all 4 attachments, draw scene items. Mirrors
    // ForwardOpaquePass shape but targets an offscreen RT instead of
    // the backbuffer / sceneFbo.
    const uint8_t viewId = kGBufferViewId;
    // Source submesh order is part of the surface-layer contract. Character
    // formats commonly encode eyes, mouth, lace and decals as coincident
    // geometry split across materials. bgfx's default view sorter may group
    // the opaque and alpha-cutout programs, destroying that order. Keep this
    // view sequential so later source submeshes deterministically replace an
    // equal-depth earlier surface.
    //
    // §P3 H1 (2026-08-24) — routed through BGFXAdapter (cutsheet red line:
    // pass files never call bgfx::* functions directly).
    ctx.adapter.setViewMode(viewId, bgfx::ViewMode::Sequential);
    ctx.adapter.setViewTransform(viewId, frame.view, frame.projection);
    ctx.adapter.setViewFrameBuffer(viewId, _gbufferFbo);
    ctx.adapter.setViewRect(viewId, 0, 0, _gbufferW, _gbufferH);
    // Clear every color attachment to transparent black. RT3.a is the
    // explicit geometry-coverage channel consumed by LightingPass, so the
    // background must start at zero; every successful GBuffer draw writes 1.
    // Using an opaque-black clear here marks the whole viewport as geometry
    // and suppresses the skybox even though SkyboxPass itself rendered.
    //
    // §P3 M4 (2026-08-24) — magic clear RGBA hoisted to
    // ayt::render::kGBufferClearRgba (0x00000000u). RT3.a's coverage
    // channel must start at zero; every draw writes 1.
    ctx.adapter.setViewClearRaw(viewId,
                                BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                                /*rgba=*/ayt::render::kGBufferClearRgba,
                                /*depth=*/1.0f,
                                /*stencil=*/0);
    // A clear-only GBuffer is still a valid produced frame (empty scene). bgfx
    // does not execute a view that has no submit unless it is touched.
    ctx.adapter.touch(viewId);

    // GBufferPass draw state: depth-write + no-blend (same as
    // ForwardOpaquePass opaque defaults — cutsheet §1.4 forward
    // fill semantics). The cached Adapter state is overwritten by
    // setStateOpaque() each pass (see ForwardOpaquePass::execute at
    // AYRenderer/src/detail/ForwardOpaquePass.cpp:189 — same line).
    ctx.adapter.setStateOpaque();

    uint32_t drawCount = 0;
    uint32_t skippedInvalidHandle = 0;
    uint32_t skippedMissingResource = 0;
    uint32_t skippedInvalidBuffer = 0;
    uint32_t skippedInvalidShader = 0;
    FrameDrawLists fallbackDrawLists;
    const FrameDrawLists& drawLists =
        resolveFrameDrawLists(ctx, fallbackDrawLists);
    uint32_t skippedTransparent = drawLists.stats.transparent3D;
    uint32_t skippedOverlay2D = drawLists.stats.overlay2D;
    uint32_t skippedEmptyRange = 0;
    uint32_t alphaCutoutCount = 0;
    uint32_t worldLit2DCount = 0;
    for (const DrawItem* itemPtr : drawLists.gbufferOpaque) {
        const DrawItem& item = *itemPtr;
        const bool worldLit2D = isWorldLit2DItem(item);
        if (!item.mesh.isValid() || !item.material.isValid()) {
            ++skippedInvalidHandle;
            continue;
        }
        const auto meshIt = ctx.meshes.find(item.mesh.id);
        const auto matIt  = ctx.materials.find(item.material.id);
        if (meshIt == ctx.meshes.end() || matIt == ctx.materials.end()) {
            ++skippedMissingResource;
            continue;
        }
        const GpuMesh& mesh = meshIt->second;
        if (!BGFXAdapter::isValid(mesh.vertexBuffer)
            || !BGFXAdapter::isValid(mesh.indexBuffer)) {
            ++skippedInvalidBuffer;
            continue;
        }
        const GpuMaterial& material = matIt->second;
        if (!material.shader.isValid()) {
            ++skippedInvalidShader;
            continue;
        }
        // Cutsheet deferred-pass.md §2: GBuffer receives Opaque only.
        // Alpha glass must not write albedo/depth here — otherwise it
        // shows as solid cyan and steals depth from real opaques.
        // TransparentPass composites Alpha after Lighting.
        // Blend/domain routing was completed once by FrameDrawLists.

        const bool needsAlphaCutout = material.alphaCutout;
        alphaCutoutCount += needsAlphaCutout ? 1u : 0u;
        if (worldLit2D && !_worldLit2DProgram.isValid()) {
            ++skippedInvalidShader;
            continue;
        }
        if (!worldLit2D && needsAlphaCutout
            && !_alphaCutoutProgram.isValid()) {
            // Drawing a cutout with the opaque fill shader writes solid depth
            // and coverage for transparent texels. A missing specialized
            // program is therefore a skipped draw, not an opaque fallback.
            ++skippedInvalidShader;
            continue;
        }
        shader::ShaderResource& drawProgram =
            worldLit2D ? _worldLit2DProgram
                       : (needsAlphaCutout ? _alphaCutoutProgram : _program);
        worldLit2DCount += worldLit2D ? 1u : 0u;

        // §P5 B4c (2026-07-22) — PREV-FRAME VP UPLOAD. Build
        // prevViewProj = prevProj * prevView (P×V same-order as
        // `setViewTransform` + `viewProjectionMatrix` builtin —
        // mirror pass-lessons-from-shadow.md §3.1 warning). On real
        // backend this triggers `bgfx::setUniform(handle, ptr, 64)`
        // for `u_prevViewProj`; on Noop the upload is a no-op
        // (ShaderResource::setUniform short-circuits).
        //
        // The current shader declares this binding for future velocity work;
        // no active MRT output consumes it yet.
        {
            const shader::BindingId prevVpBinding =
                drawProgram.getUniformBinding("u_prevViewProj");
            if (prevVpBinding != shader::InvalidBinding) {
                const ayt::math::Float4x4 prevViewProj =
                    _prevProjection * _prevView;
                float prevVpCol[16];
                toBgfxColumnMajor(prevViewProj, prevVpCol);
                drawProgram.setUniform(prevVpBinding, prevVpCol, sizeof(prevVpCol));
            }
        }

        // baseColor upload (mirror ForwardOpaquePass baseColor slot,
        // AYRenderer/src/detail/ForwardOpaquePass.cpp:42, applied via
        // the Phoskia `property baseColor` declared on GBufferFill).
        // Resolution policy identical to FO: prefer host override if
        // set, else use a neutral white tint so the GBuffer survives
        // a missing material baseColor.
        //
        // §P5 B4c (2026-07-22) — FIXED B4b bug: baseColor binding is
        // now read from `_program` (the GBufferFill Phoskia program
        // that owns the `baseColor` property uniform), NOT from
        // `material.shader` (the host material's own program). On
        // a real backend the B4b code path was uploading uniforms
        // into a different program than the one actually drawing,
        // silently dropping baseColor overrides from the GBuffer
        // fill. Read from `_program` so the binding matches the
        // submitting program below.
        const shader::BindingId baseColorBinding =
            drawProgram.getUniformBinding("baseColor");
        if (baseColorBinding != shader::InvalidBinding) {
            const float base[4] = {
                material.hasColorOverride ? material.colorOverride.x : 1.0f,
                material.hasColorOverride ? material.colorOverride.y : 1.0f,
                material.hasColorOverride ? material.colorOverride.z : 1.0f,
                material.hasColorOverride ? material.colorOverride.w : 1.0f,
            };
            drawProgram.setUniform(baseColorBinding, base, sizeof(base));
        }
        const shader::BindingId cameraBinding =
            drawProgram.getUniformBinding("cameraPos");
        if (cameraBinding != shader::InvalidBinding) {
            const float camera[4] = {
                frame.cameraPosition.x, frame.cameraPosition.y,
                frame.cameraPosition.z, 0.0f
            };
            drawProgram.setUniform(cameraBinding, camera, sizeof(camera));
        }
        trySetUniformMat4(drawProgram, "u_normalMatrix", nullptr,
                          normalMatrixForTransform(item.world));
        const shader::BindingId doubleSidedBinding =
            drawProgram.getUniformBinding("doubleSided");
        if (doubleSidedBinding != shader::InvalidBinding) {
            const float surface[4] = {
                material.doubleSided ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f
            };
            drawProgram.setUniform(doubleSidedBinding, surface, sizeof(surface));
        }
        const shader::BindingId normalYBinding =
            drawProgram.getUniformBinding("normalYSign");
        if (normalYBinding != shader::InvalidBinding) {
            float normalY = 1.0f;
            for (const GpuMaterial::UniformSlot& slot : material.uniformSlots) {
                if (slot.name == "normalYSign" && slot.size >= sizeof(float)) {
                    std::memcpy(&normalY, slot.data, sizeof(float));
                    break;
                }
            }
            const float normalConvention[4] = {normalY, 0.0f, 0.0f, 0.0f};
            drawProgram.setUniform(normalYBinding, normalConvention,
                                   sizeof(normalConvention));
        }
        const auto uploadMaterialScalar = [&](const char* name, float fallback) {
            const shader::BindingId binding = drawProgram.getUniformBinding(name);
            if (binding == shader::InvalidBinding) {
                return;
            }
            const float value[4] = {
                materialUniformScalar(material, name, fallback), 0.0f, 0.0f, 0.0f
            };
            drawProgram.setUniform(binding, value, sizeof(value));
        };
        uploadMaterialScalar("metallic", 0.0f);
        uploadMaterialScalar("roughness", 0.5f);
        uploadMaterialScalar("ao", 1.0f);
        const shader::BindingId materialModelBinding =
            drawProgram.getUniformBinding("materialModel");
        if (materialModelBinding != shader::InvalidBinding) {
            const float model[4] = {
                static_cast<float>(static_cast<uint8_t>(material.materialModel)),
                0.0f, 0.0f, 0.0f
            };
            drawProgram.setUniform(materialModelBinding, model, sizeof(model));
        }
        const shader::BindingId emissiveBinding =
            drawProgram.getUniformBinding("emissive");
        if (emissiveBinding != shader::InvalidBinding) {
            float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (const GpuMaterial::UniformSlot& slot : material.uniformSlots) {
                if (slot.name == "emissive" && slot.size >= sizeof(float) * 3u) {
                    std::memcpy(value, slot.data,
                                std::min<size_t>(sizeof(value), slot.size));
                    break;
                }
            }
            drawProgram.setUniform(emissiveBinding, value, sizeof(value));
        }
        if (needsAlphaCutout) {
            const shader::BindingId cutoffBinding =
                drawProgram.getUniformBinding("alphaCutoff");
            if (cutoffBinding != shader::InvalidBinding) {
                const float cutoff[4] = {material.alphaCutoff, 0.0f, 0.0f, 0.0f};
                drawProgram.setUniform(cutoffBinding, cutoff, sizeof(cutoff));
            }
            const shader::BindingId opacitySourceBinding =
                drawProgram.getUniformBinding("opacitySource");
            if (opacitySourceBinding != shader::InvalidBinding) {
                const float source[4] = {
                    materialUniformScalar(material, "opacitySource", 0.0f),
                    0.0f, 0.0f, 0.0f
                };
                drawProgram.setUniform(opacitySourceBinding, source,
                                       sizeof(source));
            }
            const shader::BindingId opacityBinding =
                drawProgram.getUniformBinding("opacity");
            if (opacityBinding != shader::InvalidBinding) {
                const float opacityValue[4] = {
                    materialUniformScalar(material, "opacity", 1.0f),
                    0.0f, 0.0f, 0.0f
                };
                drawProgram.setUniform(opacityBinding, opacityValue,
                                       sizeof(opacityValue));
            }
        }
        if (worldLit2D) {
            upload2DDrawUniforms(drawProgram, *item.payload);
        }

        // Forward parity: sample(albedoMap)*baseColor. Bind host
        // material texture slots onto GBufferFill's albedoMap stage
        // (skip shadowMap — GBuffer does not consume it).
        // Imported .aymat uses baseColorTexture / diffuse; GBufferFill
        // only declares albedoMap — alias those names so characters
        // are not forced to solid white.
        bool albedoBound = false;
        bool opacityBound = false;
        bool normalBound = false;
        bool metallicBound = false;
        bool roughnessBound = false;
        bool aoBound = false;
        bool emissiveBound = false;
        for (const GpuMaterial::TextureSlot& slot : material.textures) {
            if (slot.name.empty() || !slot.texture.isValid()) {
                continue;
            }
            if (slot.name == "shadowMap") {
                continue;
            }
            shader::BindingId binding = drawProgram.getTextureBinding(slot.name);
            if (binding == shader::InvalidBinding
                && (slot.name == "baseColorTexture"
                    || slot.name == "diffuse"
                    || slot.name == "mainTexture"
                    || slot.name == "albedo")) {
                binding = drawProgram.getTextureBinding("albedoMap");
            }
            if (binding == shader::InvalidBinding && needsAlphaCutout
                && slot.name == "opacityTexture") {
                binding = drawProgram.getTextureBinding("opacityMap");
            }
            if (binding == shader::InvalidBinding
                && (slot.name == "normalTexture"
                    || slot.name == "normalCameraTexture"
                    || slot.name == "normalMap")) {
                binding = drawProgram.getTextureBinding("normalMap");
            }
            if (binding == shader::InvalidBinding && slot.name == "metallicTexture") {
                binding = drawProgram.getTextureBinding("metallicMap");
            }
            if (binding == shader::InvalidBinding && slot.name == "roughnessTexture") {
                binding = drawProgram.getTextureBinding("roughnessMap");
            }
            if (binding == shader::InvalidBinding && slot.name == "aoTexture") {
                binding = drawProgram.getTextureBinding("aoMap");
            }
            if (binding == shader::InvalidBinding && slot.name == "emissiveTexture") {
                binding = drawProgram.getTextureBinding("emissiveMap");
            }
            if (binding == shader::InvalidBinding) {
                continue;
            }
            const auto texIt = ctx.textures.find(slot.texture.id);
            if (texIt == ctx.textures.end()
                || !BGFXAdapter::isValid(texIt->second.handle)) {
                continue;
            }
            const uint8_t stage = drawProgram.getTextureStage(binding);
            drawProgram.setTexture(stage, binding,
                                   toShaderTexture(texIt->second.handle));
            if (slot.name == "opacityTexture") {
                opacityBound = true;
            } else if (slot.name == "normalTexture"
                       || slot.name == "normalCameraTexture"
                       || slot.name == "normalMap") {
                normalBound = true;
            } else if (slot.name == "metallicTexture") {
                metallicBound = true;
            } else if (slot.name == "roughnessTexture") {
                roughnessBound = true;
            } else if (slot.name == "aoTexture") {
                aoBound = true;
            } else if (slot.name == "emissiveTexture") {
                emissiveBound = true;
            } else {
                albedoBound = true;
            }
        }
        tryBindWhiteTexture(drawProgram, ctx.adapter, "albedoMap", albedoBound);
        if (needsAlphaCutout) {
            tryBindWhiteTexture(drawProgram, ctx.adapter, "opacityMap", opacityBound);
        }
        tryBindFlatNormalTexture(drawProgram, ctx.adapter, "normalMap", normalBound);
        tryBindWhiteTexture(drawProgram, ctx.adapter, "metallicMap", metallicBound);
        tryBindWhiteTexture(drawProgram, ctx.adapter, "roughnessMap", roughnessBound);
        tryBindWhiteTexture(drawProgram, ctx.adapter, "aoMap", aoBound);
        tryBindWhiteTexture(drawProgram, ctx.adapter, "emissiveMap", emissiveBound);

        const DrawIndexRange drawRange = resolveDrawIndexRange(item, mesh);
        if (drawRange.indexCount == 0) {
            ++skippedEmptyRange;
            continue;
        }

        // LEQUAL is intentional here. Imported character assets frequently
        // contain exactly coincident overlay surfaces (face/eye/mouth,
        // garment/trim). LESS makes the base surface win forever; LEQUAL plus
        // Sequential preserves ordinary nearest-depth occlusion while making
        // the later source submesh the stable tie-breaker.
        //
        // §P3 M11 (2026-08-24) — routed through BGFXAdapter state preset
        // (cutsheet red line + named state helpers preferred over inline
        // bit assembly).
        ctx.adapter.setTransform(item.world);
        ctx.adapter.setVertexBuffer(mesh.vertexBuffer);
        const bool wireframe = bindDrawIndexBuffer(
            ctx.adapter, mesh, drawRange, ctx.wireframe);
        ctx.adapter.setStateOpaqueLEQUAL(
            material.doubleSided, reversesWinding(item.world), wireframe);

        if (!worldLit2D) {
            tryUploadBonePalette(
                drawProgram,
                drawProgram.getUniformBlockBinding("Skeleton"),
                drawProgram.getUniformBinding("castSkinned"),
                /*castSkinnedValue=*/1u,
                item);
        }

        shader::DrawCallContext submitCtx;
        submitCtx.viewId = viewId;
        submitCtx.state  = 0;  // state owned by Adapter; shader.submit
                                // only writes viewId (matches FO shape).
        // §P5 B4c (2026-07-22) — FIXED B4b bug: submit uses `_program`
        // (the Phoskia GBufferFill program), NOT `material.shader`
        // (the host material's program). On a real backend the B4b
        // path was submitting the host material's draws into view
        // 7, which is wrong: the GBufferFill VS/FS (which writes
        // all four GBuffer outputs to gl_FragData[0..3]) is the program
        // that MUST bind view 7, otherwise the MRT attachments stay
        // untouched. Fix: bind `_program` (GBufferFill), use the
        // host material only for per-draw state (VB/IB/world) and
        // for colorOverride fallback above.
        drawProgram.submit(submitCtx);
        ++drawCount;
    }

    static uint32_t s_routeLogFrame = 0;
    if (s_routeLogFrame < 8) {
        std::fprintf(stderr,
                     "[GBufferRoute] frame=%u items=%zu draws=%u cutout=%u "
                     "worldLit2D=%u "
                     "skip(invalidHandle=%u missing=%u buffer=%u "
                     "shader=%u transparent=%u overlay2D=%u range=%u)\n",
                     s_routeLogFrame, ctx.scene.items().size(), drawCount,
                     alphaCutoutCount, worldLit2DCount, skippedInvalidHandle,
                     skippedMissingResource, skippedInvalidBuffer,
                     skippedInvalidShader, skippedTransparent, skippedOverlay2D,
                     skippedEmptyRange);
        ++s_routeLogFrame;
    }

    _producedThisFrame = true;
    return drawCount;
}

void GBufferPass::setGbufferSize(uint16_t width, uint16_t height) noexcept
{
    // B4a: still only stores request (mirror B2 behavior — do NOT
    // call ensure here; no adapter access). Host can call this BEFORE
    // initialize() and the next execute() will honor the size.
    _gbufferW = width;
    _gbufferH = height;
    _producedThisFrame = false;
}

void GBufferPass::destroyResources(BGFXAdapter& adapter)
{
    // B4a: real cleanup (mirror ShadowMapResources::destroy
    // ShadowMapResources.cpp:128-143). All 4 attachments are owned
    // by _gbufferFbo (destroyTextures=true upstream), so resetting
    // them to BGFX_INVALID_HANDLE is enough — DO NOT call
    // bgfx::destroy on the cached attachments or you'll double-free.
    //
    // W/H + buildStamp are reset unconditionally (Test_B4_GBufferMRT
    // case 6 verifies gbufferWidth/Height return 0 after destroy even
    // when no FBO was ever allocated — a host that calls
    // setGbufferSize(800,600) → destroyResources() expects W/H back to
    // 0). The FBO handle itself is only destroyed when it was actually
    // allocated (calling bgfx::destroy on an invalid handle is a UAF
    // on some bgfx backends — see ShadowMapResources::destroy guard).
    _gbufferDepthRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferAlbedoRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferNormalRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferWorldPositionRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferMaterialRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferW = 0;
    _gbufferH = 0;
    _allocatedW = 0;
    _allocatedH = 0;
    _buildStamp = "";
    _producedThisFrame = false;
    _program.reset();
    _alphaCutoutProgram.reset();
    _worldLit2DProgram.reset();
    _acquireFailed = false;
    _alphaCutoutAcquireFailed = false;
    _worldLit2DAcquireFailed = false;
    if (bgfx::isValid(_gbufferFbo)) {
        adapter.destroy(_gbufferFbo);
        _gbufferFbo = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
    }
}

void GBufferPass::ensure(BGFXAdapter& adapter, uint16_t width, uint16_t height)
{
    // Mirror ShadowMapResources::ensure (ShadowMapResources.cpp:93-126).
    //
    // Stamp-changed fast path: when `buildStamp` differs from
    // `_buildStamp`, force rebuild. Currently we always pass
    // kGBufferBuildStamp so stamp is stable across frames — keeps
    // the cache hot until a cutsheet-bump forces a rebuild.
    if (!adapter.isInitialized() || width == 0 || height == 0) {
        return;
    }

    const bool stampChanged = (_buildStamp != kGBufferBuildStamp);
    if (stampChanged) {
        _buildStamp = kGBufferBuildStamp;
    }

    // Fast path: allocated FBO size matches request (NOT _gbufferW —
    // setGbufferSize already wrote the request into those fields).
    if (bgfx::isValid(_gbufferFbo)
        && _allocatedW == width
        && _allocatedH == height
        && !stampChanged) {
        if (!bgfx::isValid(_gbufferAlbedoRt)) {
            cacheAttachments(adapter);
        }
        return;
    }

    // Rebuild path — destroy old FBO + reset cache + create new.
    if (bgfx::isValid(_gbufferFbo)) {
        adapter.destroy(_gbufferFbo);
        _gbufferFbo       = bgfx::FrameBufferHandle{BGFX_INVALID_HANDLE};
        _gbufferAlbedoRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferNormalRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferWorldPositionRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferMaterialRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _gbufferDepthRt   = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
        _allocatedW = _allocatedH = 0;
    }

    _gbufferFbo = adapter.createGbufferFrameBuffer(width, height);
    if (bgfx::isValid(_gbufferFbo)) {
        _allocatedW = width;
        _allocatedH = height;
        _gbufferW = width;
        _gbufferH = height;
        cacheAttachments(adapter);
    }
}

void GBufferPass::cacheAttachments(BGFXAdapter& adapter)
{
    // Mirror ShadowMapResources::cacheColorAttachment
    // (ShadowMapResources.cpp:14-21). All 4 attachments are owned by
    // _gbufferFbo (destroyTextures=true), so we read them via
    // adapter.getFboAttachment and never call bgfx::destroy on them.
    _gbufferAlbedoRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferNormalRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferWorldPositionRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferMaterialRt = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    _gbufferDepthRt  = bgfx::TextureHandle{BGFX_INVALID_HANDLE};
    if (!bgfx::isValid(_gbufferFbo)) {
        return;
    }
    _gbufferAlbedoRt = adapter.getFboAttachment(
        _gbufferFbo, GBufferLayout::kAlbedoMetallicAttachment);
    _gbufferNormalRt = adapter.getFboAttachment(
        _gbufferFbo, GBufferLayout::kNormalRoughnessAttachment);
    _gbufferWorldPositionRt = adapter.getFboAttachment(
        _gbufferFbo, GBufferLayout::kWorldPositionPackedAttachment);
    _gbufferMaterialRt = adapter.getFboAttachment(
        _gbufferFbo, GBufferLayout::kEmissiveCoverageAttachment);
    _gbufferDepthRt = adapter.getFboAttachment(
        _gbufferFbo, GBufferLayout::kDepthAttachment);
}

} // namespace ayt::render::detail
