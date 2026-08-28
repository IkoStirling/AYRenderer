#pragma once

namespace ayt::render
{

// Runtime PBR material used by imported meshes. Keep
// demo/assets/pbr.phoskia byte-for-byte equivalent; the standalone asset is
// shipped for tools/cookers while this embedded copy lets Editor seed a fresh
// runtime cache without depending on its working directory.
inline constexpr const char* kPbrPhoskiaSource = R"PHOSKIA(
uniformblock Skeleton {
    mat4 bones[128]
} binding 0
uniformblock Lights {
    vec4 dirs[8]
    vec4 colors[8]
    vec4 params[8]
    vec4 spotDir[8]
} binding 1
material PBR {
    texture2d baseColorTexture
    texture2d opacityTexture
    texture2d normalTexture
    texture2d metallicTexture
    texture2d roughnessTexture
    texture2d aoTexture
    texture2d emissiveTexture
    texture2d shadowMap
    texturecube envCube

    uniform mat4 u_lightViewProj
    uniform vec4 cameraPos
    uniform vec4 lightDir
    uniform vec4 lightColor
    uniform vec4 shadowMapTexel
    uniform vec4 shadowAtlasRects[8]
    uniform mat4 lightViewProjs[8]
    uniform vec4 shadowBiases[8]
    uniform vec4 perLightShadowCount
    uniform vec4 activeLightCount
    uniform vec4 cubeActive
    uniform vec4 ambientStrength

    property baseColor  = vec4(1.0, 1.0, 1.0, 1.0)
    property metallic   = vec4(0.0, 0.0, 0.0, 0.0)
    property roughness  = vec4(0.5, 0.0, 0.0, 0.0)
    property ao         = vec4(1.0, 0.0, 0.0, 0.0)
    property emissive   = vec4(0.0, 0.0, 0.0, 0.0)
    property opacity    = vec4(1.0, 0.0, 0.0, 0.0)
    // 0 = BaseColor alpha only, 1 = dedicated opacity red,
    // 2 = dedicated opacity alpha.
    property opacitySource = vec4(0.0, 0.0, 0.0, 0.0)
    property doubleSided = vec4(0.0, 0.0, 0.0, 0.0)
    property normalYSign = vec4(1.0, 0.0, 0.0, 0.0)
    property premultipliedAlpha = vec4(0.0, 0.0, 0.0, 0.0)
    property shadowBias = vec4(0.003, 0.0, 0.0, 0.0)
    property shadowPcf  = vec4(1.0, 0.0, 0.0, 0.0)
    property castSkinned = vec4(0.0, 0.0, 0.0, 0.0)

    vertex {
        in pos : position
        in nrm : normal
        in tan : tangent
        in uv  : texcoord
        in boneId : boneindices
        in boneWt : boneweights
        out worldNormal : normal = (modelMatrix * mix(
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
        out uvOut : texcoord = uv
        return modelViewProjection * mix(
            vec4(pos, 1.0),
            skinningMatrix(boneId, boneWt, Skeleton.bones, vec4(pos, 1.0)),
            castSkinned.x)
    }

    fragment {
        in worldNormal : normal
        in worldTangent : tangent
        in worldPos : position
        in uvOut : texcoord

        let sampledBase = sample(baseColorTexture, uvOut) * baseColor
        let sampledOpacityTexture = sample(opacityTexture, uvOut)
        let dedicatedOpacity = mix(sampledOpacityTexture.x,
                                   sampledOpacityTexture.w,
                                   step(1.5, opacitySource.x))
        let sampledOpacity = mix(1.0, dedicatedOpacity,
                                 step(0.5, opacitySource.x))
        let albedo = sampledBase.rgb
        let V = normalize(cameraPos.xyz - worldPos)
        let rawN = normalize(worldNormal)
        let tangentSeed = worldTangent.xyz - rawN * dot(worldTangent.xyz, rawN)
                         + vec3(0.000001, 0.0, 0.0)
        let T = normalize(tangentSeed)
        let B = normalize(cross(rawN, T)) * worldTangent.w
        let tangentNormal = sample(normalTexture, uvOut).xyz * 2.0
                          - vec3(1.0, 1.0, 1.0)
        let mappedN = normalize(T * tangentNormal.x
                              + B * tangentNormal.y * normalYSign.x
                              + rawN * tangentNormal.z)
        let faceSign = mix(1.0, step(0.0, dot(mappedN, V)) * 2.0 - 1.0,
                           max(0.0, min(1.0, doubleSided.x)))
        let N = mappedN * faceSign
        let materialMetallic = max(0.0, min(1.0,
            metallic.x * sample(metallicTexture, uvOut).x))
        let materialRoughness = max(0.045, min(1.0,
            roughness.x * sample(roughnessTexture, uvOut).x))
        let materialAo = max(0.0, min(1.0,
            ao.x * sample(aoTexture, uvOut).x))
        let NdotV = max(dot(N, V), 0.001)
        let materialF0 = mix(vec3(0.04, 0.04, 0.04), albedo, materialMetallic)

        // Light 0
        let dirL0 = Lights.dirs[0].xyz
        let Ld0 = dirL0 * (1.0 / max(length(dirL0), 0.0001))
        let toL0 = Lights.dirs[0].xyz - worldPos
        let dist0 = length(toL0)
        let Lp0 = toL0 * (1.0 / max(dist0, 0.0001))
        let spotN0 = Lights.spotDir[0].xyz
                       * (1.0 / max(length(Lights.spotDir[0].xyz), 0.0001))
        let cone0 = smoothstep(Lights.params[0].w,
                                  Lights.params[0].z,
                                  -1.0 * dot(Lp0, spotN0))
        let falloff0 = 1.0 - smoothstep(0.0, Lights.params[0].x, dist0)
        let pointAtten0 = Lights.params[0].y * falloff0
                           / max(dist0 * dist0, 0.01)
        let spotAtten0 = pointAtten0 * cone0
        let isDir0 = 1.0 - step(0.5, Lights.dirs[0].w)
        let isPoint0 = step(0.5, Lights.dirs[0].w)
                         - step(1.5, Lights.dirs[0].w)
        let isSpot0 = step(1.5, Lights.dirs[0].w)
        let active0 = step(0.5, activeLightCount.x)
        let rawL0 = Ld0 * isDir0
                     + Lp0 * (isPoint0 + isSpot0)
        let L0 = rawL0 * (1.0 / max(length(rawL0), 0.0001))
        let rawH0 = V + L0
        let H0 = rawH0 * (1.0 / max(length(rawH0), 0.0001))
        let NdotL0 = max(dot(N, L0), 0.0)

        let clip0 = lightViewProjs[0] * vec4(worldPos, 1.0)
        let invW0 = 1.0 / max(clip0.w, 0.0001)
        let localUv0 = vec2(clip0.x * invW0 * 0.5 + 0.5,
                               1.0 - (clip0.y * invW0 * 0.5 + 0.5))
        let refDepth0 = clip0.z * invW0 * 0.5 + 0.5
        let atlasUv0 = mix(shadowAtlasRects[0].xy,
                              shadowAtlasRects[0].zw,
                              localUv0)
        let safeUv0 = vec2(
            max(shadowAtlasRects[0].x + shadowMapTexel.x * 1.5,
                min(shadowAtlasRects[0].z - shadowMapTexel.x * 1.5,
                    atlasUv0.x)),
            max(shadowAtlasRects[0].y + shadowMapTexel.y * 1.5,
                min(shadowAtlasRects[0].w - shadowMapTexel.y * 1.5,
                    atlasUv0.y)))
        let inShadowMap0 = step(0.0001, clip0.w)
                           * step(0.0, localUv0.x) * step(localUv0.x, 1.0)
                           * step(0.0, localUv0.y) * step(localUv0.y, 1.0)
                           * step(0.0, refDepth0) * step(refDepth0, 1.0)
        let slotBias0 = shadowBiases[0].x
                         * step(0.0001, shadowBiases[0].x)
                         + shadowBias.x
                         * (1.0 - step(0.0001, shadowBiases[0].x))
        let depth0a = sample(shadowMap,
            safeUv0 + vec2(-shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth0b = sample(shadowMap,
            safeUv0 + vec2( shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth0c = sample(shadowMap,
            safeUv0 + vec2(-shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth0d = sample(shadowMap,
            safeUv0 + vec2( shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth0e = sample(shadowMap, safeUv0).x
        let lit0a = max(1.0 - step(depth0a + slotBias0, refDepth0),
                           step(0.999, depth0a))
        let lit0b = max(1.0 - step(depth0b + slotBias0, refDepth0),
                           step(0.999, depth0b))
        let lit0c = max(1.0 - step(depth0c + slotBias0, refDepth0),
                           step(0.999, depth0c))
        let lit0d = max(1.0 - step(depth0d + slotBias0, refDepth0),
                           step(0.999, depth0d))
        let lit0e = max(1.0 - step(depth0e + slotBias0, refDepth0),
                           step(0.999, depth0e))
        let softShadow0 = (lit0a + lit0b + lit0c + lit0d) * 0.25
        let sampledShadow0 = mix(lit0e, softShadow0, shadowPcf.x)
        let shadow0 = mix(
            1.0,
            mix(1.0, sampledShadow0, inShadowMap0),
            step(0.5, perLightShadowCount.x))

        let radiance0 = Lights.colors[0].xyz
                         * (isDir0
                            + isPoint0 * pointAtten0
                            + isSpot0 * spotAtten0)
                         * active0
        let fresnel0 = fresnelSchlick(max(dot(V, H0), 0.0), materialF0)
        let D0 = distributionGGX(max(dot(N, H0), 0.0),
                                    materialRoughness)
        let G0 = geometrySmith(NdotV, NdotL0, materialRoughness)
        let diffuse0 = (vec3(1.0, 1.0, 1.0) - fresnel0)
                         * (1.0 - materialMetallic)
                         * albedo * (1.0 / 3.14159265)
        let specular0 = fresnel0
                          * (D0 * G0
                             / max(4.0 * NdotV * NdotL0, 0.001))
        let direct0 = (diffuse0 + specular0)
                        * radiance0 * NdotL0 * shadow0

        // Light 1
        let dirL1 = Lights.dirs[1].xyz
        let Ld1 = dirL1 * (1.0 / max(length(dirL1), 0.0001))
        let toL1 = Lights.dirs[1].xyz - worldPos
        let dist1 = length(toL1)
        let Lp1 = toL1 * (1.0 / max(dist1, 0.0001))
        let spotN1 = Lights.spotDir[1].xyz
                       * (1.0 / max(length(Lights.spotDir[1].xyz), 0.0001))
        let cone1 = smoothstep(Lights.params[1].w,
                                  Lights.params[1].z,
                                  -1.0 * dot(Lp1, spotN1))
        let falloff1 = 1.0 - smoothstep(0.0, Lights.params[1].x, dist1)
        let pointAtten1 = Lights.params[1].y * falloff1
                           / max(dist1 * dist1, 0.01)
        let spotAtten1 = pointAtten1 * cone1
        let isDir1 = 1.0 - step(0.5, Lights.dirs[1].w)
        let isPoint1 = step(0.5, Lights.dirs[1].w)
                         - step(1.5, Lights.dirs[1].w)
        let isSpot1 = step(1.5, Lights.dirs[1].w)
        let active1 = step(1.5, activeLightCount.x)
        let rawL1 = Ld1 * isDir1
                     + Lp1 * (isPoint1 + isSpot1)
        let L1 = rawL1 * (1.0 / max(length(rawL1), 0.0001))
        let rawH1 = V + L1
        let H1 = rawH1 * (1.0 / max(length(rawH1), 0.0001))
        let NdotL1 = max(dot(N, L1), 0.0)

        let clip1 = lightViewProjs[1] * vec4(worldPos, 1.0)
        let invW1 = 1.0 / max(clip1.w, 0.0001)
        let localUv1 = vec2(clip1.x * invW1 * 0.5 + 0.5,
                               1.0 - (clip1.y * invW1 * 0.5 + 0.5))
        let refDepth1 = clip1.z * invW1 * 0.5 + 0.5
        let atlasUv1 = mix(shadowAtlasRects[1].xy,
                              shadowAtlasRects[1].zw,
                              localUv1)
        let safeUv1 = vec2(
            max(shadowAtlasRects[1].x + shadowMapTexel.x * 1.5,
                min(shadowAtlasRects[1].z - shadowMapTexel.x * 1.5,
                    atlasUv1.x)),
            max(shadowAtlasRects[1].y + shadowMapTexel.y * 1.5,
                min(shadowAtlasRects[1].w - shadowMapTexel.y * 1.5,
                    atlasUv1.y)))
        let inShadowMap1 = step(0.0001, clip1.w)
                           * step(0.0, localUv1.x) * step(localUv1.x, 1.0)
                           * step(0.0, localUv1.y) * step(localUv1.y, 1.0)
                           * step(0.0, refDepth1) * step(refDepth1, 1.0)
        let slotBias1 = shadowBiases[1].x
                         * step(0.0001, shadowBiases[1].x)
                         + shadowBias.x
                         * (1.0 - step(0.0001, shadowBiases[1].x))
        let depth1a = sample(shadowMap,
            safeUv1 + vec2(-shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth1b = sample(shadowMap,
            safeUv1 + vec2( shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth1c = sample(shadowMap,
            safeUv1 + vec2(-shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth1d = sample(shadowMap,
            safeUv1 + vec2( shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth1e = sample(shadowMap, safeUv1).x
        let lit1a = max(1.0 - step(depth1a + slotBias1, refDepth1),
                           step(0.999, depth1a))
        let lit1b = max(1.0 - step(depth1b + slotBias1, refDepth1),
                           step(0.999, depth1b))
        let lit1c = max(1.0 - step(depth1c + slotBias1, refDepth1),
                           step(0.999, depth1c))
        let lit1d = max(1.0 - step(depth1d + slotBias1, refDepth1),
                           step(0.999, depth1d))
        let lit1e = max(1.0 - step(depth1e + slotBias1, refDepth1),
                           step(0.999, depth1e))
        let softShadow1 = (lit1a + lit1b + lit1c + lit1d) * 0.25
        let sampledShadow1 = mix(lit1e, softShadow1, shadowPcf.x)
        let shadow1 = mix(
            1.0,
            mix(1.0, sampledShadow1, inShadowMap1),
            step(1.5, perLightShadowCount.x))

        let radiance1 = Lights.colors[1].xyz
                         * (isDir1
                            + isPoint1 * pointAtten1
                            + isSpot1 * spotAtten1)
                         * active1
        let fresnel1 = fresnelSchlick(max(dot(V, H1), 0.0), materialF0)
        let D1 = distributionGGX(max(dot(N, H1), 0.0),
                                    materialRoughness)
        let G1 = geometrySmith(NdotV, NdotL1, materialRoughness)
        let diffuse1 = (vec3(1.0, 1.0, 1.0) - fresnel1)
                         * (1.0 - materialMetallic)
                         * albedo * (1.0 / 3.14159265)
        let specular1 = fresnel1
                          * (D1 * G1
                             / max(4.0 * NdotV * NdotL1, 0.001))
        let direct1 = (diffuse1 + specular1)
                        * radiance1 * NdotL1 * shadow1

        // Light 2
        let dirL2 = Lights.dirs[2].xyz
        let Ld2 = dirL2 * (1.0 / max(length(dirL2), 0.0001))
        let toL2 = Lights.dirs[2].xyz - worldPos
        let dist2 = length(toL2)
        let Lp2 = toL2 * (1.0 / max(dist2, 0.0001))
        let spotN2 = Lights.spotDir[2].xyz
                       * (1.0 / max(length(Lights.spotDir[2].xyz), 0.0001))
        let cone2 = smoothstep(Lights.params[2].w,
                                  Lights.params[2].z,
                                  -1.0 * dot(Lp2, spotN2))
        let falloff2 = 1.0 - smoothstep(0.0, Lights.params[2].x, dist2)
        let pointAtten2 = Lights.params[2].y * falloff2
                           / max(dist2 * dist2, 0.01)
        let spotAtten2 = pointAtten2 * cone2
        let isDir2 = 1.0 - step(0.5, Lights.dirs[2].w)
        let isPoint2 = step(0.5, Lights.dirs[2].w)
                         - step(1.5, Lights.dirs[2].w)
        let isSpot2 = step(1.5, Lights.dirs[2].w)
        let active2 = step(2.5, activeLightCount.x)
        let rawL2 = Ld2 * isDir2
                     + Lp2 * (isPoint2 + isSpot2)
        let L2 = rawL2 * (1.0 / max(length(rawL2), 0.0001))
        let rawH2 = V + L2
        let H2 = rawH2 * (1.0 / max(length(rawH2), 0.0001))
        let NdotL2 = max(dot(N, L2), 0.0)

        let clip2 = lightViewProjs[2] * vec4(worldPos, 1.0)
        let invW2 = 1.0 / max(clip2.w, 0.0001)
        let localUv2 = vec2(clip2.x * invW2 * 0.5 + 0.5,
                               1.0 - (clip2.y * invW2 * 0.5 + 0.5))
        let refDepth2 = clip2.z * invW2 * 0.5 + 0.5
        let atlasUv2 = mix(shadowAtlasRects[2].xy,
                              shadowAtlasRects[2].zw,
                              localUv2)
        let safeUv2 = vec2(
            max(shadowAtlasRects[2].x + shadowMapTexel.x * 1.5,
                min(shadowAtlasRects[2].z - shadowMapTexel.x * 1.5,
                    atlasUv2.x)),
            max(shadowAtlasRects[2].y + shadowMapTexel.y * 1.5,
                min(shadowAtlasRects[2].w - shadowMapTexel.y * 1.5,
                    atlasUv2.y)))
        let inShadowMap2 = step(0.0001, clip2.w)
                           * step(0.0, localUv2.x) * step(localUv2.x, 1.0)
                           * step(0.0, localUv2.y) * step(localUv2.y, 1.0)
                           * step(0.0, refDepth2) * step(refDepth2, 1.0)
        let slotBias2 = shadowBiases[2].x
                         * step(0.0001, shadowBiases[2].x)
                         + shadowBias.x
                         * (1.0 - step(0.0001, shadowBiases[2].x))
        let depth2a = sample(shadowMap,
            safeUv2 + vec2(-shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth2b = sample(shadowMap,
            safeUv2 + vec2( shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth2c = sample(shadowMap,
            safeUv2 + vec2(-shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth2d = sample(shadowMap,
            safeUv2 + vec2( shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth2e = sample(shadowMap, safeUv2).x
        let lit2a = max(1.0 - step(depth2a + slotBias2, refDepth2),
                           step(0.999, depth2a))
        let lit2b = max(1.0 - step(depth2b + slotBias2, refDepth2),
                           step(0.999, depth2b))
        let lit2c = max(1.0 - step(depth2c + slotBias2, refDepth2),
                           step(0.999, depth2c))
        let lit2d = max(1.0 - step(depth2d + slotBias2, refDepth2),
                           step(0.999, depth2d))
        let lit2e = max(1.0 - step(depth2e + slotBias2, refDepth2),
                           step(0.999, depth2e))
        let softShadow2 = (lit2a + lit2b + lit2c + lit2d) * 0.25
        let sampledShadow2 = mix(lit2e, softShadow2, shadowPcf.x)
        let shadow2 = mix(
            1.0,
            mix(1.0, sampledShadow2, inShadowMap2),
            step(2.5, perLightShadowCount.x))

        let radiance2 = Lights.colors[2].xyz
                         * (isDir2
                            + isPoint2 * pointAtten2
                            + isSpot2 * spotAtten2)
                         * active2
        let fresnel2 = fresnelSchlick(max(dot(V, H2), 0.0), materialF0)
        let D2 = distributionGGX(max(dot(N, H2), 0.0),
                                    materialRoughness)
        let G2 = geometrySmith(NdotV, NdotL2, materialRoughness)
        let diffuse2 = (vec3(1.0, 1.0, 1.0) - fresnel2)
                         * (1.0 - materialMetallic)
                         * albedo * (1.0 / 3.14159265)
        let specular2 = fresnel2
                          * (D2 * G2
                             / max(4.0 * NdotV * NdotL2, 0.001))
        let direct2 = (diffuse2 + specular2)
                        * radiance2 * NdotL2 * shadow2

        // Light 3
        let dirL3 = Lights.dirs[3].xyz
        let Ld3 = dirL3 * (1.0 / max(length(dirL3), 0.0001))
        let toL3 = Lights.dirs[3].xyz - worldPos
        let dist3 = length(toL3)
        let Lp3 = toL3 * (1.0 / max(dist3, 0.0001))
        let spotN3 = Lights.spotDir[3].xyz
                       * (1.0 / max(length(Lights.spotDir[3].xyz), 0.0001))
        let cone3 = smoothstep(Lights.params[3].w,
                                  Lights.params[3].z,
                                  -1.0 * dot(Lp3, spotN3))
        let falloff3 = 1.0 - smoothstep(0.0, Lights.params[3].x, dist3)
        let pointAtten3 = Lights.params[3].y * falloff3
                           / max(dist3 * dist3, 0.01)
        let spotAtten3 = pointAtten3 * cone3
        let isDir3 = 1.0 - step(0.5, Lights.dirs[3].w)
        let isPoint3 = step(0.5, Lights.dirs[3].w)
                         - step(1.5, Lights.dirs[3].w)
        let isSpot3 = step(1.5, Lights.dirs[3].w)
        let active3 = step(3.5, activeLightCount.x)
        let rawL3 = Ld3 * isDir3
                     + Lp3 * (isPoint3 + isSpot3)
        let L3 = rawL3 * (1.0 / max(length(rawL3), 0.0001))
        let rawH3 = V + L3
        let H3 = rawH3 * (1.0 / max(length(rawH3), 0.0001))
        let NdotL3 = max(dot(N, L3), 0.0)

        let clip3 = lightViewProjs[3] * vec4(worldPos, 1.0)
        let invW3 = 1.0 / max(clip3.w, 0.0001)
        let localUv3 = vec2(clip3.x * invW3 * 0.5 + 0.5,
                               1.0 - (clip3.y * invW3 * 0.5 + 0.5))
        let refDepth3 = clip3.z * invW3 * 0.5 + 0.5
        let atlasUv3 = mix(shadowAtlasRects[3].xy,
                              shadowAtlasRects[3].zw,
                              localUv3)
        let safeUv3 = vec2(
            max(shadowAtlasRects[3].x + shadowMapTexel.x * 1.5,
                min(shadowAtlasRects[3].z - shadowMapTexel.x * 1.5,
                    atlasUv3.x)),
            max(shadowAtlasRects[3].y + shadowMapTexel.y * 1.5,
                min(shadowAtlasRects[3].w - shadowMapTexel.y * 1.5,
                    atlasUv3.y)))
        let inShadowMap3 = step(0.0001, clip3.w)
                           * step(0.0, localUv3.x) * step(localUv3.x, 1.0)
                           * step(0.0, localUv3.y) * step(localUv3.y, 1.0)
                           * step(0.0, refDepth3) * step(refDepth3, 1.0)
        let slotBias3 = shadowBiases[3].x
                         * step(0.0001, shadowBiases[3].x)
                         + shadowBias.x
                         * (1.0 - step(0.0001, shadowBiases[3].x))
        let depth3a = sample(shadowMap,
            safeUv3 + vec2(-shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth3b = sample(shadowMap,
            safeUv3 + vec2( shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth3c = sample(shadowMap,
            safeUv3 + vec2(-shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth3d = sample(shadowMap,
            safeUv3 + vec2( shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth3e = sample(shadowMap, safeUv3).x
        let lit3a = max(1.0 - step(depth3a + slotBias3, refDepth3),
                           step(0.999, depth3a))
        let lit3b = max(1.0 - step(depth3b + slotBias3, refDepth3),
                           step(0.999, depth3b))
        let lit3c = max(1.0 - step(depth3c + slotBias3, refDepth3),
                           step(0.999, depth3c))
        let lit3d = max(1.0 - step(depth3d + slotBias3, refDepth3),
                           step(0.999, depth3d))
        let lit3e = max(1.0 - step(depth3e + slotBias3, refDepth3),
                           step(0.999, depth3e))
        let softShadow3 = (lit3a + lit3b + lit3c + lit3d) * 0.25
        let sampledShadow3 = mix(lit3e, softShadow3, shadowPcf.x)
        let shadow3 = mix(
            1.0,
            mix(1.0, sampledShadow3, inShadowMap3),
            step(3.5, perLightShadowCount.x))

        let radiance3 = Lights.colors[3].xyz
                         * (isDir3
                            + isPoint3 * pointAtten3
                            + isSpot3 * spotAtten3)
                         * active3
        let fresnel3 = fresnelSchlick(max(dot(V, H3), 0.0), materialF0)
        let D3 = distributionGGX(max(dot(N, H3), 0.0),
                                    materialRoughness)
        let G3 = geometrySmith(NdotV, NdotL3, materialRoughness)
        let diffuse3 = (vec3(1.0, 1.0, 1.0) - fresnel3)
                         * (1.0 - materialMetallic)
                         * albedo * (1.0 / 3.14159265)
        let specular3 = fresnel3
                          * (D3 * G3
                             / max(4.0 * NdotV * NdotL3, 0.001))
        let direct3 = (diffuse3 + specular3)
                        * radiance3 * NdotL3 * shadow3

        // Light 4
        let dirL4 = Lights.dirs[4].xyz
        let Ld4 = dirL4 * (1.0 / max(length(dirL4), 0.0001))
        let toL4 = Lights.dirs[4].xyz - worldPos
        let dist4 = length(toL4)
        let Lp4 = toL4 * (1.0 / max(dist4, 0.0001))
        let spotN4 = Lights.spotDir[4].xyz
                       * (1.0 / max(length(Lights.spotDir[4].xyz), 0.0001))
        let cone4 = smoothstep(Lights.params[4].w,
                                  Lights.params[4].z,
                                  -1.0 * dot(Lp4, spotN4))
        let falloff4 = 1.0 - smoothstep(0.0, Lights.params[4].x, dist4)
        let pointAtten4 = Lights.params[4].y * falloff4
                           / max(dist4 * dist4, 0.01)
        let spotAtten4 = pointAtten4 * cone4
        let isDir4 = 1.0 - step(0.5, Lights.dirs[4].w)
        let isPoint4 = step(0.5, Lights.dirs[4].w)
                         - step(1.5, Lights.dirs[4].w)
        let isSpot4 = step(1.5, Lights.dirs[4].w)
        let active4 = step(4.5, activeLightCount.x)
        let rawL4 = Ld4 * isDir4
                     + Lp4 * (isPoint4 + isSpot4)
        let L4 = rawL4 * (1.0 / max(length(rawL4), 0.0001))
        let rawH4 = V + L4
        let H4 = rawH4 * (1.0 / max(length(rawH4), 0.0001))
        let NdotL4 = max(dot(N, L4), 0.0)

        let clip4 = lightViewProjs[4] * vec4(worldPos, 1.0)
        let invW4 = 1.0 / max(clip4.w, 0.0001)
        let localUv4 = vec2(clip4.x * invW4 * 0.5 + 0.5,
                               1.0 - (clip4.y * invW4 * 0.5 + 0.5))
        let refDepth4 = clip4.z * invW4 * 0.5 + 0.5
        let atlasUv4 = mix(shadowAtlasRects[4].xy,
                              shadowAtlasRects[4].zw,
                              localUv4)
        let safeUv4 = vec2(
            max(shadowAtlasRects[4].x + shadowMapTexel.x * 1.5,
                min(shadowAtlasRects[4].z - shadowMapTexel.x * 1.5,
                    atlasUv4.x)),
            max(shadowAtlasRects[4].y + shadowMapTexel.y * 1.5,
                min(shadowAtlasRects[4].w - shadowMapTexel.y * 1.5,
                    atlasUv4.y)))
        let inShadowMap4 = step(0.0001, clip4.w)
                           * step(0.0, localUv4.x) * step(localUv4.x, 1.0)
                           * step(0.0, localUv4.y) * step(localUv4.y, 1.0)
                           * step(0.0, refDepth4) * step(refDepth4, 1.0)
        let slotBias4 = shadowBiases[4].x
                         * step(0.0001, shadowBiases[4].x)
                         + shadowBias.x
                         * (1.0 - step(0.0001, shadowBiases[4].x))
        let depth4a = sample(shadowMap,
            safeUv4 + vec2(-shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth4b = sample(shadowMap,
            safeUv4 + vec2( shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth4c = sample(shadowMap,
            safeUv4 + vec2(-shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth4d = sample(shadowMap,
            safeUv4 + vec2( shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth4e = sample(shadowMap, safeUv4).x
        let lit4a = max(1.0 - step(depth4a + slotBias4, refDepth4),
                           step(0.999, depth4a))
        let lit4b = max(1.0 - step(depth4b + slotBias4, refDepth4),
                           step(0.999, depth4b))
        let lit4c = max(1.0 - step(depth4c + slotBias4, refDepth4),
                           step(0.999, depth4c))
        let lit4d = max(1.0 - step(depth4d + slotBias4, refDepth4),
                           step(0.999, depth4d))
        let lit4e = max(1.0 - step(depth4e + slotBias4, refDepth4),
                           step(0.999, depth4e))
        let softShadow4 = (lit4a + lit4b + lit4c + lit4d) * 0.25
        let sampledShadow4 = mix(lit4e, softShadow4, shadowPcf.x)
        let shadow4 = mix(
            1.0,
            mix(1.0, sampledShadow4, inShadowMap4),
            step(4.5, perLightShadowCount.x))

        let radiance4 = Lights.colors[4].xyz
                         * (isDir4
                            + isPoint4 * pointAtten4
                            + isSpot4 * spotAtten4)
                         * active4
        let fresnel4 = fresnelSchlick(max(dot(V, H4), 0.0), materialF0)
        let D4 = distributionGGX(max(dot(N, H4), 0.0),
                                    materialRoughness)
        let G4 = geometrySmith(NdotV, NdotL4, materialRoughness)
        let diffuse4 = (vec3(1.0, 1.0, 1.0) - fresnel4)
                         * (1.0 - materialMetallic)
                         * albedo * (1.0 / 3.14159265)
        let specular4 = fresnel4
                          * (D4 * G4
                             / max(4.0 * NdotV * NdotL4, 0.001))
        let direct4 = (diffuse4 + specular4)
                        * radiance4 * NdotL4 * shadow4

        // Light 5
        let dirL5 = Lights.dirs[5].xyz
        let Ld5 = dirL5 * (1.0 / max(length(dirL5), 0.0001))
        let toL5 = Lights.dirs[5].xyz - worldPos
        let dist5 = length(toL5)
        let Lp5 = toL5 * (1.0 / max(dist5, 0.0001))
        let spotN5 = Lights.spotDir[5].xyz
                       * (1.0 / max(length(Lights.spotDir[5].xyz), 0.0001))
        let cone5 = smoothstep(Lights.params[5].w,
                                  Lights.params[5].z,
                                  -1.0 * dot(Lp5, spotN5))
        let falloff5 = 1.0 - smoothstep(0.0, Lights.params[5].x, dist5)
        let pointAtten5 = Lights.params[5].y * falloff5
                           / max(dist5 * dist5, 0.01)
        let spotAtten5 = pointAtten5 * cone5
        let isDir5 = 1.0 - step(0.5, Lights.dirs[5].w)
        let isPoint5 = step(0.5, Lights.dirs[5].w)
                         - step(1.5, Lights.dirs[5].w)
        let isSpot5 = step(1.5, Lights.dirs[5].w)
        let active5 = step(5.5, activeLightCount.x)
        let rawL5 = Ld5 * isDir5
                     + Lp5 * (isPoint5 + isSpot5)
        let L5 = rawL5 * (1.0 / max(length(rawL5), 0.0001))
        let rawH5 = V + L5
        let H5 = rawH5 * (1.0 / max(length(rawH5), 0.0001))
        let NdotL5 = max(dot(N, L5), 0.0)

        let clip5 = lightViewProjs[5] * vec4(worldPos, 1.0)
        let invW5 = 1.0 / max(clip5.w, 0.0001)
        let localUv5 = vec2(clip5.x * invW5 * 0.5 + 0.5,
                               1.0 - (clip5.y * invW5 * 0.5 + 0.5))
        let refDepth5 = clip5.z * invW5 * 0.5 + 0.5
        let atlasUv5 = mix(shadowAtlasRects[5].xy,
                              shadowAtlasRects[5].zw,
                              localUv5)
        let safeUv5 = vec2(
            max(shadowAtlasRects[5].x + shadowMapTexel.x * 1.5,
                min(shadowAtlasRects[5].z - shadowMapTexel.x * 1.5,
                    atlasUv5.x)),
            max(shadowAtlasRects[5].y + shadowMapTexel.y * 1.5,
                min(shadowAtlasRects[5].w - shadowMapTexel.y * 1.5,
                    atlasUv5.y)))
        let inShadowMap5 = step(0.0001, clip5.w)
                           * step(0.0, localUv5.x) * step(localUv5.x, 1.0)
                           * step(0.0, localUv5.y) * step(localUv5.y, 1.0)
                           * step(0.0, refDepth5) * step(refDepth5, 1.0)
        let slotBias5 = shadowBiases[5].x
                         * step(0.0001, shadowBiases[5].x)
                         + shadowBias.x
                         * (1.0 - step(0.0001, shadowBiases[5].x))
        let depth5a = sample(shadowMap,
            safeUv5 + vec2(-shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth5b = sample(shadowMap,
            safeUv5 + vec2( shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth5c = sample(shadowMap,
            safeUv5 + vec2(-shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth5d = sample(shadowMap,
            safeUv5 + vec2( shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth5e = sample(shadowMap, safeUv5).x
        let lit5a = max(1.0 - step(depth5a + slotBias5, refDepth5),
                           step(0.999, depth5a))
        let lit5b = max(1.0 - step(depth5b + slotBias5, refDepth5),
                           step(0.999, depth5b))
        let lit5c = max(1.0 - step(depth5c + slotBias5, refDepth5),
                           step(0.999, depth5c))
        let lit5d = max(1.0 - step(depth5d + slotBias5, refDepth5),
                           step(0.999, depth5d))
        let lit5e = max(1.0 - step(depth5e + slotBias5, refDepth5),
                           step(0.999, depth5e))
        let softShadow5 = (lit5a + lit5b + lit5c + lit5d) * 0.25
        let sampledShadow5 = mix(lit5e, softShadow5, shadowPcf.x)
        let shadow5 = mix(
            1.0,
            mix(1.0, sampledShadow5, inShadowMap5),
            step(5.5, perLightShadowCount.x))

        let radiance5 = Lights.colors[5].xyz
                         * (isDir5
                            + isPoint5 * pointAtten5
                            + isSpot5 * spotAtten5)
                         * active5
        let fresnel5 = fresnelSchlick(max(dot(V, H5), 0.0), materialF0)
        let D5 = distributionGGX(max(dot(N, H5), 0.0),
                                    materialRoughness)
        let G5 = geometrySmith(NdotV, NdotL5, materialRoughness)
        let diffuse5 = (vec3(1.0, 1.0, 1.0) - fresnel5)
                         * (1.0 - materialMetallic)
                         * albedo * (1.0 / 3.14159265)
        let specular5 = fresnel5
                          * (D5 * G5
                             / max(4.0 * NdotV * NdotL5, 0.001))
        let direct5 = (diffuse5 + specular5)
                        * radiance5 * NdotL5 * shadow5

        // Light 6
        let dirL6 = Lights.dirs[6].xyz
        let Ld6 = dirL6 * (1.0 / max(length(dirL6), 0.0001))
        let toL6 = Lights.dirs[6].xyz - worldPos
        let dist6 = length(toL6)
        let Lp6 = toL6 * (1.0 / max(dist6, 0.0001))
        let spotN6 = Lights.spotDir[6].xyz
                       * (1.0 / max(length(Lights.spotDir[6].xyz), 0.0001))
        let cone6 = smoothstep(Lights.params[6].w,
                                  Lights.params[6].z,
                                  -1.0 * dot(Lp6, spotN6))
        let falloff6 = 1.0 - smoothstep(0.0, Lights.params[6].x, dist6)
        let pointAtten6 = Lights.params[6].y * falloff6
                           / max(dist6 * dist6, 0.01)
        let spotAtten6 = pointAtten6 * cone6
        let isDir6 = 1.0 - step(0.5, Lights.dirs[6].w)
        let isPoint6 = step(0.5, Lights.dirs[6].w)
                         - step(1.5, Lights.dirs[6].w)
        let isSpot6 = step(1.5, Lights.dirs[6].w)
        let active6 = step(6.5, activeLightCount.x)
        let rawL6 = Ld6 * isDir6
                     + Lp6 * (isPoint6 + isSpot6)
        let L6 = rawL6 * (1.0 / max(length(rawL6), 0.0001))
        let rawH6 = V + L6
        let H6 = rawH6 * (1.0 / max(length(rawH6), 0.0001))
        let NdotL6 = max(dot(N, L6), 0.0)

        let clip6 = lightViewProjs[6] * vec4(worldPos, 1.0)
        let invW6 = 1.0 / max(clip6.w, 0.0001)
        let localUv6 = vec2(clip6.x * invW6 * 0.5 + 0.5,
                               1.0 - (clip6.y * invW6 * 0.5 + 0.5))
        let refDepth6 = clip6.z * invW6 * 0.5 + 0.5
        let atlasUv6 = mix(shadowAtlasRects[6].xy,
                              shadowAtlasRects[6].zw,
                              localUv6)
        let safeUv6 = vec2(
            max(shadowAtlasRects[6].x + shadowMapTexel.x * 1.5,
                min(shadowAtlasRects[6].z - shadowMapTexel.x * 1.5,
                    atlasUv6.x)),
            max(shadowAtlasRects[6].y + shadowMapTexel.y * 1.5,
                min(shadowAtlasRects[6].w - shadowMapTexel.y * 1.5,
                    atlasUv6.y)))
        let inShadowMap6 = step(0.0001, clip6.w)
                           * step(0.0, localUv6.x) * step(localUv6.x, 1.0)
                           * step(0.0, localUv6.y) * step(localUv6.y, 1.0)
                           * step(0.0, refDepth6) * step(refDepth6, 1.0)
        let slotBias6 = shadowBiases[6].x
                         * step(0.0001, shadowBiases[6].x)
                         + shadowBias.x
                         * (1.0 - step(0.0001, shadowBiases[6].x))
        let depth6a = sample(shadowMap,
            safeUv6 + vec2(-shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth6b = sample(shadowMap,
            safeUv6 + vec2( shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth6c = sample(shadowMap,
            safeUv6 + vec2(-shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth6d = sample(shadowMap,
            safeUv6 + vec2( shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth6e = sample(shadowMap, safeUv6).x
        let lit6a = max(1.0 - step(depth6a + slotBias6, refDepth6),
                           step(0.999, depth6a))
        let lit6b = max(1.0 - step(depth6b + slotBias6, refDepth6),
                           step(0.999, depth6b))
        let lit6c = max(1.0 - step(depth6c + slotBias6, refDepth6),
                           step(0.999, depth6c))
        let lit6d = max(1.0 - step(depth6d + slotBias6, refDepth6),
                           step(0.999, depth6d))
        let lit6e = max(1.0 - step(depth6e + slotBias6, refDepth6),
                           step(0.999, depth6e))
        let softShadow6 = (lit6a + lit6b + lit6c + lit6d) * 0.25
        let sampledShadow6 = mix(lit6e, softShadow6, shadowPcf.x)
        let shadow6 = mix(
            1.0,
            mix(1.0, sampledShadow6, inShadowMap6),
            step(6.5, perLightShadowCount.x))

        let radiance6 = Lights.colors[6].xyz
                         * (isDir6
                            + isPoint6 * pointAtten6
                            + isSpot6 * spotAtten6)
                         * active6
        let fresnel6 = fresnelSchlick(max(dot(V, H6), 0.0), materialF0)
        let D6 = distributionGGX(max(dot(N, H6), 0.0),
                                    materialRoughness)
        let G6 = geometrySmith(NdotV, NdotL6, materialRoughness)
        let diffuse6 = (vec3(1.0, 1.0, 1.0) - fresnel6)
                         * (1.0 - materialMetallic)
                         * albedo * (1.0 / 3.14159265)
        let specular6 = fresnel6
                          * (D6 * G6
                             / max(4.0 * NdotV * NdotL6, 0.001))
        let direct6 = (diffuse6 + specular6)
                        * radiance6 * NdotL6 * shadow6

        // Light 7
        let dirL7 = Lights.dirs[7].xyz
        let Ld7 = dirL7 * (1.0 / max(length(dirL7), 0.0001))
        let toL7 = Lights.dirs[7].xyz - worldPos
        let dist7 = length(toL7)
        let Lp7 = toL7 * (1.0 / max(dist7, 0.0001))
        let spotN7 = Lights.spotDir[7].xyz
                       * (1.0 / max(length(Lights.spotDir[7].xyz), 0.0001))
        let cone7 = smoothstep(Lights.params[7].w,
                                  Lights.params[7].z,
                                  -1.0 * dot(Lp7, spotN7))
        let falloff7 = 1.0 - smoothstep(0.0, Lights.params[7].x, dist7)
        let pointAtten7 = Lights.params[7].y * falloff7
                           / max(dist7 * dist7, 0.01)
        let spotAtten7 = pointAtten7 * cone7
        let isDir7 = 1.0 - step(0.5, Lights.dirs[7].w)
        let isPoint7 = step(0.5, Lights.dirs[7].w)
                         - step(1.5, Lights.dirs[7].w)
        let isSpot7 = step(1.5, Lights.dirs[7].w)
        let active7 = step(7.5, activeLightCount.x)
        let rawL7 = Ld7 * isDir7
                     + Lp7 * (isPoint7 + isSpot7)
        let L7 = rawL7 * (1.0 / max(length(rawL7), 0.0001))
        let rawH7 = V + L7
        let H7 = rawH7 * (1.0 / max(length(rawH7), 0.0001))
        let NdotL7 = max(dot(N, L7), 0.0)

        let clip7 = lightViewProjs[7] * vec4(worldPos, 1.0)
        let invW7 = 1.0 / max(clip7.w, 0.0001)
        let localUv7 = vec2(clip7.x * invW7 * 0.5 + 0.5,
                               1.0 - (clip7.y * invW7 * 0.5 + 0.5))
        let refDepth7 = clip7.z * invW7 * 0.5 + 0.5
        let atlasUv7 = mix(shadowAtlasRects[7].xy,
                              shadowAtlasRects[7].zw,
                              localUv7)
        let safeUv7 = vec2(
            max(shadowAtlasRects[7].x + shadowMapTexel.x * 1.5,
                min(shadowAtlasRects[7].z - shadowMapTexel.x * 1.5,
                    atlasUv7.x)),
            max(shadowAtlasRects[7].y + shadowMapTexel.y * 1.5,
                min(shadowAtlasRects[7].w - shadowMapTexel.y * 1.5,
                    atlasUv7.y)))
        let inShadowMap7 = step(0.0001, clip7.w)
                           * step(0.0, localUv7.x) * step(localUv7.x, 1.0)
                           * step(0.0, localUv7.y) * step(localUv7.y, 1.0)
                           * step(0.0, refDepth7) * step(refDepth7, 1.0)
        let slotBias7 = shadowBiases[7].x
                         * step(0.0001, shadowBiases[7].x)
                         + shadowBias.x
                         * (1.0 - step(0.0001, shadowBiases[7].x))
        let depth7a = sample(shadowMap,
            safeUv7 + vec2(-shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth7b = sample(shadowMap,
            safeUv7 + vec2( shadowMapTexel.x, -shadowMapTexel.y)).x
        let depth7c = sample(shadowMap,
            safeUv7 + vec2(-shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth7d = sample(shadowMap,
            safeUv7 + vec2( shadowMapTexel.x,  shadowMapTexel.y)).x
        let depth7e = sample(shadowMap, safeUv7).x
        let lit7a = max(1.0 - step(depth7a + slotBias7, refDepth7),
                           step(0.999, depth7a))
        let lit7b = max(1.0 - step(depth7b + slotBias7, refDepth7),
                           step(0.999, depth7b))
        let lit7c = max(1.0 - step(depth7c + slotBias7, refDepth7),
                           step(0.999, depth7c))
        let lit7d = max(1.0 - step(depth7d + slotBias7, refDepth7),
                           step(0.999, depth7d))
        let lit7e = max(1.0 - step(depth7e + slotBias7, refDepth7),
                           step(0.999, depth7e))
        let softShadow7 = (lit7a + lit7b + lit7c + lit7d) * 0.25
        let sampledShadow7 = mix(lit7e, softShadow7, shadowPcf.x)
        let shadow7 = mix(
            1.0,
            mix(1.0, sampledShadow7, inShadowMap7),
            step(7.5, perLightShadowCount.x))

        let radiance7 = Lights.colors[7].xyz
                         * (isDir7
                            + isPoint7 * pointAtten7
                            + isSpot7 * spotAtten7)
                         * active7
        let fresnel7 = fresnelSchlick(max(dot(V, H7), 0.0), materialF0)
        let D7 = distributionGGX(max(dot(N, H7), 0.0),
                                    materialRoughness)
        let G7 = geometrySmith(NdotV, NdotL7, materialRoughness)
        let diffuse7 = (vec3(1.0, 1.0, 1.0) - fresnel7)
                         * (1.0 - materialMetallic)
                         * albedo * (1.0 / 3.14159265)
        let specular7 = fresnel7
                          * (D7 * G7
                             / max(4.0 * NdotV * NdotL7, 0.001))
        let direct7 = (diffuse7 + specular7)
                        * radiance7 * NdotL7 * shadow7

        let direct = direct0 + direct1 + direct2 + direct3
                   + direct4 + direct5 + direct6 + direct7
        let flatAmbient = albedo * (0.03 * materialAo)
        let cubeAmbient = sample(envCube, N).rgb * albedo
                        * (ambientStrength.x * cubeActive.x * materialAo)
        let materialEmissive = emissive.xyz * sample(emissiveTexture, uvOut).rgb
        let color = flatAmbient + cubeAmbient + direct + materialEmissive
        let alpha = sampledBase.a * sampledOpacity
                  * max(0.0, min(1.0, opacity.x))
        let outputColor = mix(color, color * alpha,
                              max(0.0, min(1.0, premultipliedAlpha.x)))
        return vec4(outputColor, alpha)
    }
}
)PHOSKIA";

} // namespace ayt::render
