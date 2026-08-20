#pragma once

namespace ayt::render
{

// Runtime PBR material used by imported meshes. Keep
// demo/assets/pbr.phoskia byte-for-byte equivalent; the standalone asset is
// shipped for tools/cookers while this embedded copy lets Editor seed a fresh
// runtime cache without depending on its working directory.
inline constexpr const char* kPbrPhoskiaSource = R"PHOSKIA(
material PBR {
    texture2d baseColorTexture
    texture2d shadowMap

    uniform mat4 u_lightViewProj
    uniform vec4 cameraPos
    uniform vec4 lightDir
    uniform vec4 lightColor
    uniform vec4 shadowMapTexel

    property baseColor  = vec4(1.0, 1.0, 1.0, 1.0)
    property metallic   = vec4(0.0, 0.0, 0.0, 0.0)
    property roughness  = vec4(0.5, 0.0, 0.0, 0.0)
    property ao         = vec4(1.0, 0.0, 0.0, 0.0)
    property emissive   = vec4(0.0, 0.0, 0.0, 0.0)
    property opacity    = vec4(1.0, 0.0, 0.0, 0.0)
    property shadowBias = vec4(0.003, 0.0, 0.0, 0.0)
    property shadowPcf  = vec4(1.0, 0.0, 0.0, 0.0)

    vertex {
        in pos : position
        in nrm : normal
        in uv  : texcoord
        out worldNormal : normal   = (modelMatrix * vec4(nrm, 0.0)).xyz
        out worldPos    : position = (modelMatrix * vec4(pos, 1.0)).xyz
        out uvOut       : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }

    fragment {
        in worldNormal : normal
        in worldPos    : position
        in uvOut       : texcoord

        let sampledBase = sample(baseColorTexture, uvOut) * baseColor
        let albedo = sampledBase.rgb
        let N = normalize(worldNormal)
        let V = normalize(cameraPos.xyz - worldPos)
        let L = normalize(lightDir.xyz)
        let H = normalize(V + L)

        let materialMetallic = max(0.0, min(1.0, metallic.x))
        let materialRoughness = max(0.045, min(1.0, roughness.x))
        let materialAo = max(0.0, min(1.0, ao.x))
        let NdotV = max(dot(N, V), 0.001)
        let NdotL = max(dot(N, L), 0.0)
        let NdotH = max(dot(N, H), 0.0)
        let VdotH = max(dot(V, H), 0.0)

        let F0 = mix(vec3(0.04, 0.04, 0.04), albedo, materialMetallic)
        let F = fresnelSchlick(VdotH, F0)
        let D = distributionGGX(NdotH, materialRoughness)
        let G = geometrySmith(NdotV, NdotL, materialRoughness)
        let specular = D * G * F / max(4.0 * NdotV * NdotL, 0.001)
        let diffuse = (vec3(1.0, 1.0, 1.0) - F) * (1.0 - materialMetallic)
                    * albedo * (1.0 / 3.14159265)

        let clipPos  = u_lightViewProj * vec4(worldPos, 1.0)
        let invW     = 1.0 / max(clipPos.w, 0.0001)
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
        let o11 = sample(shadowMap, shadowUv).x
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
        let shadow = mix(1.0, mix(s11, shadowSoft, shadowPcf.x), inMap)

        let direct = (diffuse + specular) * lightColor.xyz * NdotL * shadow
        let ambient = albedo * (0.03 * materialAo)
        let color = ambient + direct + emissive.xyz
        let alpha = sampledBase.a * max(0.0, min(1.0, opacity.x))
        return vec4(color, alpha)
    }
}
)PHOSKIA";

} // namespace ayt::render
