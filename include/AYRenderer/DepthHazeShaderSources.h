#pragma once

namespace ayt::render
{

inline constexpr const char kDepthHazeCacheKey[] =
    "depthhaze_v4_fullres_coverage_fs";

// Produces a full-resolution, scene-linear opaque result. Geometry coverage
// makes the background policy explicit: uncovered pixels represent infinity
// and therefore receive the full requested haze amount. SSAO is already folded
// into Lighting's ambient term, so this pass only performs haze composition.
inline constexpr const char kDepthHazePhoskiaSource[] = R"PHOSKIA(
material DepthHaze {
    texture2d sceneColor
    texture2d worldPosition
    texture2d geometryCoverage
    uniform vec4 hazeDensity
    uniform vec4 hazeStrength
    uniform vec4 hazeColor
    uniform vec4 camPos
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let raw = sample(sceneColor, uv)
        let worldPos = sample(worldPosition, uv).xyz
        let coverage = step(0.5, sample(geometryCoverage, uv).a)
        let dist = length(worldPos - camPos.xyz)
        let surfaceFog = clamp(1.0 - exp(-hazeDensity.x * dist), 0.0, 1.0)
        let fogFactor = clamp(
            mix(1.0, surfaceFog, coverage) * hazeStrength.x,
            0.0,
            1.0)
        let mixed = mix(raw.xyz, hazeColor.xyz, fogFactor)
        return vec4(mixed, raw.w)
    }
}
)PHOSKIA";

} // namespace ayt::render
