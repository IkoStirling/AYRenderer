#pragma once
namespace ayt::render {
inline constexpr char kSsrCacheKey[] = "ssr_v3_explicit_scene_matrices";
inline constexpr char kSsrPhoskiaSource[] = R"PHOSKIA(
material ScreenSpaceReflection {
    texture2d sceneColor
    texture2d worldPosition
    texture2d geometryCoverage
    texture2d normalRoughness
    texture2d albedoMetallic
    texture2d ssaoTexture
    texturecube prefilteredSpecularCube
    texture2d brdfLut
    // Fullscreen view builtins are identity, not the scene camera.
    uniform mat4 ssrViewMatrix
    uniform mat4 ssrProjectionMatrix
    uniform vec4 cameraPosition
    uniform vec4 traceParams
    uniform vec4 materialParams
    uniform vec4 iblParams
    uniform vec4 ssaoParams
    vertex {
        in pos : position
        out vUv : texcoord = pos.xy * vec2(0.5) + vec2(0.5)
        return vec4(pos.xy, 0.0, 1.0)
    }
    fragment {
        in vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let raw = sample(sceneColor, uv)
        let wp = sample(worldPosition, uv)
        let surface = sample(geometryCoverage, uv)
        let nr = sample(normalRoughness, uv)
        let albedo = sample(albedoMetallic, uv)
        let model = floor(wp.a * 0.5 + 0.0001)
        let result = raw
        if (surface.a >= 0.5 && model < 0.5 && nr.a < materialParams.y) {
        let N = normalize(nr.xyz * 2.0 - vec3(1.0))
        let V = normalize(cameraPosition.xyz - wp.xyz)
        let R = reflect(V * -1.0, N)
        let origin = (ssrViewMatrix * vec4(wp.xyz + N * traceParams.z, 1.0)).xyz
        let direction = normalize((ssrViewMatrix * vec4(R, 0.0)).xyz)
        let stride = traceParams.y / traceParams.w
        let found = 0.0
        let confidence = 0.0
        let hitColor = vec3(0.0)
        let previousDelta = -traceParams.z
        for (i in range(int(traceParams.w), 64)) {
            let ray = origin + direction * (float(i + 1) * stride)
            let clip = ssrProjectionMatrix * vec4(ray, 1.0)
            if (found < 0.5 && clip.w > 0.0001 && ray.z > 0.0) {
                let ndc = clip.xy / clip.w
                let hitUv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)
                if (hitUv.x > 0.0 && hitUv.x < 1.0 && hitUv.y > 0.0 && hitUv.y < 1.0) {
                    let hitCoverage = sampleLod(geometryCoverage, hitUv, 0.0).a
                    let hitWorld = sampleLod(worldPosition, hitUv, 0.0).xyz
                    let sceneZ = (ssrViewMatrix * vec4(hitWorld, 1.0)).z
                    let delta = ray.z - sceneZ
                    // Bracket crossings only, with a finite thickness. Never accept
                    // a ray already behind the first sampled surface or a sky texel.
                    if (hitCoverage > 0.5 && previousDelta < 0.0 && delta >= 0.0 && delta < traceParams.z + abs(direction.z) * stride) {
                        let low = max(float(i) * stride, 0.0)
                        let high = float(i + 1) * stride
                        let refinedUv = hitUv
                        for (j in range(4)) {
                            let mid = (low + high) * 0.5
                            let refinedRay = origin + direction * mid
                            let refinedClip = ssrProjectionMatrix * vec4(refinedRay, 1.0)
                            let q = refinedClip.xy / max(refinedClip.w, 0.0001)
                            refinedUv = vec2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5)
                            let refinedWorld = sampleLod(worldPosition, refinedUv, 0.0).xyz
                            let refinedZ = (ssrViewMatrix * vec4(refinedWorld, 1.0)).z
                            if (refinedRay.z >= refinedZ) { high = mid } else { low = mid }
                        }
                        let hitNormal = normalize(sampleLod(normalRoughness, refinedUv, 0.0).xyz * 2.0 - vec3(1.0))
                        let finalWorld = sampleLod(worldPosition, refinedUv, 0.0).xyz
                        let finalCoverage = sampleLod(geometryCoverage, refinedUv, 0.0).a
                        let finalRay = origin + direction * high
                        let finalZ = (ssrViewMatrix * vec4(finalWorld, 1.0)).z
                        let validHit = step(0.5, finalCoverage)
                                     * (1.0 - step(traceParams.z, abs(finalRay.z - finalZ)))
                                     * step(traceParams.z * 2.0, length(finalWorld - wp.xyz))
                        let border = min(min(refinedUv.x, 1.0 - refinedUv.x), min(refinedUv.y, 1.0 - refinedUv.y))
                        let facing = smoothstep(0.0, 0.2, dot(hitNormal, R * -1.0))
                        confidence = smoothstep(0.0, 0.08, border) * facing * validHit
                                   * (1.0 - float(i + 1) / traceParams.w)
                                   * (1.0 - smoothstep(materialParams.y * 0.65, materialParams.y, nr.a))
                        hitColor = sampleLod(sceneColor, refinedUv, 0.0).rgb
                        // A rejected self hit must not terminate the ray before
                        // it reaches a different receiver farther along the screen.
                        found = validHit
                    }
                    previousDelta = delta
                } else { found = 1.0 }
            }
        }
        let roughness = max(nr.a, 0.045)
        let NdotV = max(dot(N, V), 0.001)
        let F0 = mix(vec3(0.04), albedo.rgb, clamp(albedo.a, 0.0, 1.0))
        let F = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0)
        let brdf = sample(brdfLut, vec2(NdotV, roughness)).xy
        let brdfWeight = F * brdf.x + vec3(brdf.y)
        let oldSpecular = sampleLod(prefilteredSpecularCube, R, roughness * iblParams.y).rgb
        let ao0 = sample(ssaoTexture, uv)
        let ao1 = sample(ssaoTexture, uv + vec2(-ssaoParams.y, 0.0))
        let ao2 = sample(ssaoTexture, uv + vec2(ssaoParams.y, 0.0))
        let ao3 = sample(ssaoTexture, uv + vec2(0.0, -ssaoParams.z))
        let ao4 = sample(ssaoTexture, uv + vec2(0.0, ssaoParams.z))
        let aoSum = ao0.a + ao1.a + ao2.a + ao3.a + ao4.a
        let occlusion = clamp((ao0.r * ao0.a + ao1.r * ao1.a + ao2.r * ao2.a + ao3.r * ao3.a + ao4.r * ao4.a) / max(aoSum, 1.0), 0.0, 1.0)
        let ambientAo = clamp(wp.a - model * 2.0, 0.0, 1.0) * clamp(1.0 - occlusion * ssaoParams.x, 0.0, 1.0)
        let weight = confidence * traceParams.x
        // Replace precisely the existing split-sum IBL specular on valid hits;
        // preserve direct light, diffuse, emissive, AO and miss fallback.
        if (materialParams.z > 1.5) { result = vec4(hitColor * confidence, raw.a) }
        else if (materialParams.z > 0.5) { result = vec4(vec3(confidence), raw.a) }
        else { result = vec4(max(raw.rgb + (hitColor - oldSpecular) * brdfWeight * iblParams.x * ambientAo * weight, vec3(0.0)), raw.a) }
        } else if (materialParams.z > 0.5) { result = vec4(0.0, 0.0, 0.0, raw.a) }
        return result
    }
}
)PHOSKIA";
}
