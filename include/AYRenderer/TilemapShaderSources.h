#pragma once

namespace ayt::render
{

// CM-1 (2026-08-11) — 2D lane shader source, shared by the tilemap
// and sprite paths (a sprite is a "source rect = authored rect" single
// quad). Compiles at runtime through the standard phoskia →
// AYShadercDriver → shaderc.exe → bgfx Program chain — zero CMake
// involvement (same shape as kSimpleLitShadowPhoskiaSource in
// AYRenderer/ShadowShaderSources.h).
//
// Per-draw uniforms (uploaded by Forward2DOpaquePass::execute from
// the DrawPayload2D):
//   - srcRect : UV-space source rect (min corner, max corner)
//   - tint    : per-instance color
//   - flip    : x = flip horizontal, y = flip vertical
//
// V-axis convention: AY2D tileUV is origin-bottom-left
// (AY2D/TileSamplerUV.h:75-80), so the fragment flips V once before
// sampling — tile source rects authored with row 0 = bottom land
// correctly.
inline constexpr const char* kTilemapPhoskiaSource = R"(
material Tilemap2D {
    texture2d albedoMap
    property srcRect = vec4(0.0, 0.0, 1.0, 1.0)
    property tint    = vec4(1.0, 1.0, 1.0, 1.0)
    property flip    = vec4(0.0, 0.0, 0.0, 0.0)

    vertex {
        in pos : position
        in uv  : texcoord
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in uvOut : texcoord
        let v = 1.0 - uvOut.y
        let u = mix(uvOut.x, 1.0 - uvOut.x, flip.x)
        let vv = mix(v, 1.0 - v, flip.y)
        let uvInRect = vec2(mix(srcRect.x, srcRect.z, u), mix(srcRect.y, srcRect.w, vv))
        let albedo = sample(albedoMap, uvInRect) * tint
        return vec4(albedo.rgb, albedo.a)
    }
}
)";

} // namespace ayt::render
