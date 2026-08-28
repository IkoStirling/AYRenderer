#pragma once

namespace ayt::render
{

inline constexpr const char kBloomExtractCacheKey[] =
    "bloomextract_v2_hdr_karis_softknee_fs";

inline constexpr const char kBloomExtractPhoskiaSource[] = R"(
material BloomExtract {
    texture2d sceneColor
    uniform vec4 bloomThreshold
    uniform vec4 sourceTexelSize
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let texel = sourceTexelSize.xy

        // Karis-weighted downsample suppresses isolated HDR fireflies before
        // the half-resolution blur. All source paths use linear sampling.
        let s0 = sample(sceneColor, uv)
        let s1 = sample(sceneColor, uv + texel * vec2(-1.0, -1.0))
        let s2 = sample(sceneColor, uv + texel * vec2( 1.0, -1.0))
        let s3 = sample(sceneColor, uv + texel * vec2(-1.0,  1.0))
        let s4 = sample(sceneColor, uv + texel * vec2( 1.0,  1.0))
        let w0 = 1.0 / (1.0 + max(max(s0.x, s0.y), s0.z))
        let w1 = 1.0 / (1.0 + max(max(s1.x, s1.y), s1.z))
        let w2 = 1.0 / (1.0 + max(max(s2.x, s2.y), s2.z))
        let w3 = 1.0 / (1.0 + max(max(s3.x, s3.y), s3.z))
        let w4 = 1.0 / (1.0 + max(max(s4.x, s4.y), s4.z))
        let weightSum = max(w0 + w1 + w2 + w3 + w4, 0.0001)
        let sampled = (s0 * w0 + s1 * w1 + s2 * w2 + s3 * w3 + s4 * w4)
                    * (1.0 / weightSum)

        // Scene-linear HDR threshold with a configurable fractional knee.
        let lum = dot(sampled.xyz, vec3(0.2126, 0.7152, 0.0722))
        let threshold = bloomThreshold.x
        let knee = max(threshold * bloomThreshold.y, 0.0001)
        let softBase = clamp(lum - threshold + knee, 0.0, 2.0 * knee)
        let soft = softBase * softBase / (4.0 * knee + 0.0001)
        let contribution = max(lum - threshold, soft) / max(lum, 0.0001)
        let outRgb = sampled.xyz * contribution
        return vec4(outRgb, 0.0)
    }
}
)";

inline constexpr const char kBloomBlurCacheKey[] =
    "bloomblur_v2_bilinear_5fetch_fs";

inline constexpr const char kBloomBlurPhoskiaSource[] = R"(
material BloomBlur {
    texture2d source
    uniform vec4 direction
    uniform vec4 texelSize
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let offset = direction.xy * texelSize.xy
        // Linear filtering folds each adjacent Gaussian pair into one fetch:
        // five samples per axis instead of the previous nine.
        let center = sample(source, uv) * 0.2270270270
        let nearP = sample(source, uv + offset * 1.3846153846) * 0.3162162162
        let nearN = sample(source, uv - offset * 1.3846153846) * 0.3162162162
        let farP = sample(source, uv + offset * 3.2307692308) * 0.0702702703
        let farN = sample(source, uv - offset * 3.2307692308) * 0.0702702703
        let result = center + nearP + nearN + farP + farN
        return vec4(result.x, result.y, result.z, 0.0)
    }
}
)";

} // namespace ayt::render
