#pragma once

namespace ayt::render
{

inline constexpr const char kAutoExposureCacheKey[] =
    "auto_exposure_v1_log_luminance_temporal_fs";

inline constexpr const char kAutoExposurePhoskiaSource[] = R"(
material AutoExposure {
    texture2d sceneColor
    texture2d previousExposure
    uniform vec4 exposureRange
    uniform vec4 adaptation
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let c00 = sample(sceneColor, vec2(0.125, 0.125)).xyz
        let c10 = sample(sceneColor, vec2(0.375, 0.125)).xyz
        let c20 = sample(sceneColor, vec2(0.625, 0.125)).xyz
        let c30 = sample(sceneColor, vec2(0.875, 0.125)).xyz
        let c01 = sample(sceneColor, vec2(0.125, 0.375)).xyz
        let c11 = sample(sceneColor, vec2(0.375, 0.375)).xyz
        let c21 = sample(sceneColor, vec2(0.625, 0.375)).xyz
        let c31 = sample(sceneColor, vec2(0.875, 0.375)).xyz
        let c02 = sample(sceneColor, vec2(0.125, 0.625)).xyz
        let c12 = sample(sceneColor, vec2(0.375, 0.625)).xyz
        let c22 = sample(sceneColor, vec2(0.625, 0.625)).xyz
        let c32 = sample(sceneColor, vec2(0.875, 0.625)).xyz
        let c03 = sample(sceneColor, vec2(0.125, 0.875)).xyz
        let c13 = sample(sceneColor, vec2(0.375, 0.875)).xyz
        let c23 = sample(sceneColor, vec2(0.625, 0.875)).xyz
        let c33 = sample(sceneColor, vec2(0.875, 0.875)).xyz
        let luma = vec3(0.2126, 0.7152, 0.0722)
        let logSum = log(max(dot(c00, luma), 0.0001))
                   + log(max(dot(c10, luma), 0.0001))
                   + log(max(dot(c20, luma), 0.0001))
                   + log(max(dot(c30, luma), 0.0001))
                   + log(max(dot(c01, luma), 0.0001))
                   + log(max(dot(c11, luma), 0.0001))
                   + log(max(dot(c21, luma), 0.0001))
                   + log(max(dot(c31, luma), 0.0001))
                   + log(max(dot(c02, luma), 0.0001))
                   + log(max(dot(c12, luma), 0.0001))
                   + log(max(dot(c22, luma), 0.0001))
                   + log(max(dot(c32, luma), 0.0001))
                   + log(max(dot(c03, luma), 0.0001))
                   + log(max(dot(c13, luma), 0.0001))
                   + log(max(dot(c23, luma), 0.0001))
                   + log(max(dot(c33, luma), 0.0001))
        let averageLuminance = exp(logSum * 0.0625)
        let target = clamp(exposureRange.x / max(averageLuminance, 0.0001),
                           exposureRange.y, exposureRange.z)
        let previous = sample(previousExposure, vec2(0.5, 0.5)).x
        let brighten = step(previous, target)
        let alpha = mix(adaptation.y, adaptation.x, brighten)
        let temporal = mix(previous, target, alpha)
        let result = mix(target, temporal, adaptation.z)
        return vec4(result, averageLuminance, target, 1.0)
    }
}
)";

} // namespace ayt::render
