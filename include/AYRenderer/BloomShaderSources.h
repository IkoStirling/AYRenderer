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

// Bloom v2 uses a real mip-like pyramid rather than repeatedly blurring one
// half-resolution image. The downsample kernel is deliberately energy
// preserving; the first Karis-filtered threshold stage already removed HDR
// fireflies.
inline constexpr const char kBloomPyramidDownsampleCacheKey[] =
    "bloom_pyramid_downsample_v1_13tap_fs";

inline constexpr const char kBloomPyramidDownsamplePhoskiaSource[] = R"(
material BloomPyramidDownsample {
    texture2d source
    uniform vec4 sourceTexelSize
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let t = sourceTexelSize.xy
        let center = sample(source, uv) * 0.125
        let axial = (sample(source, uv + vec2( t.x, 0.0))
                   + sample(source, uv + vec2(-t.x, 0.0))
                   + sample(source, uv + vec2(0.0,  t.y))
                   + sample(source, uv + vec2(0.0, -t.y))) * 0.125
        let diagonal = (sample(source, uv + vec2( t.x,  t.y))
                      + sample(source, uv + vec2(-t.x,  t.y))
                      + sample(source, uv + vec2( t.x, -t.y))
                      + sample(source, uv + vec2(-t.x, -t.y))) * 0.0625
        let wide = (sample(source, uv + vec2(2.0 * t.x, 0.0))
                  + sample(source, uv + vec2(-2.0 * t.x, 0.0))
                  + sample(source, uv + vec2(0.0, 2.0 * t.y))
                  + sample(source, uv + vec2(0.0, -2.0 * t.y))) * 0.03125
        let result = center + axial + diagonal + wide
        return vec4(result.xyz, 0.0)
    }
}
)";

// A 3x3 tent reconstructs the lower level and adds it to the matching
// higher-frequency level. bloomCombine=(lowWeight, highWeight, 0, 0) also
// lets the final resolve apply the tent without adding a second image.
inline constexpr const char kBloomPyramidUpsampleCacheKey[] =
    "bloom_pyramid_upsample_v1_tent_combine_fs";

inline constexpr const char kBloomPyramidUpsamplePhoskiaSource[] = R"(
material BloomPyramidUpsample {
    texture2d lowSource
    texture2d highSource
    uniform vec4 lowTexelSize
    uniform vec4 bloomCombine
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let t = lowTexelSize.xy
        let corners = sample(lowSource, uv + vec2(-t.x, -t.y))
                    + sample(lowSource, uv + vec2( t.x, -t.y))
                    + sample(lowSource, uv + vec2(-t.x,  t.y))
                    + sample(lowSource, uv + vec2( t.x,  t.y))
        let edges = sample(lowSource, uv + vec2(-t.x, 0.0))
                  + sample(lowSource, uv + vec2( t.x, 0.0))
                  + sample(lowSource, uv + vec2(0.0, -t.y))
                  + sample(lowSource, uv + vec2(0.0,  t.y))
        let tent = (corners + edges * 2.0 + sample(lowSource, uv) * 4.0)
                 * 0.0625
        let high = sample(highSource, uv)
        let result = tent * bloomCombine.x + high * bloomCombine.y
        return vec4(result.xyz, 0.0)
    }
}
)";

} // namespace ayt::render
