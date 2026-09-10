#pragma once

#include "AYRenderer/RenderTypes.h"

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

// Chunk-mesh variants sample baked atlas UVs directly. `atlasTexel.xy` is the
// inverse atlas size uploaded per draw. The 4/9-tap variants are opt-in quality
// filters for scaled/rotated presentation; Linear is the normal default.
inline constexpr const char* kTilemapChunkNearestPhoskiaSource = R"(
material TilemapChunkNearest {
    texture2d albedoMap
    uniform vec4 atlasTexel
    property tint = vec4(1.0, 1.0, 1.0, 1.0)
    vertex {
        in pos : position
        in uv : texcoord
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in uvOut : texcoord
        let snapped = vec2((floor(uvOut.x / atlasTexel.x) + 0.5) * atlasTexel.x,
                           (floor(uvOut.y / atlasTexel.y) + 0.5) * atlasTexel.y)
        return sample(albedoMap, snapped) * tint
    }
}
)";

inline constexpr const char* kTilemapChunkLinearPhoskiaSource = R"(
material TilemapChunkLinear {
    texture2d albedoMap
    uniform vec4 atlasTexel
    property tint = vec4(1.0, 1.0, 1.0, 1.0)
    vertex {
        in pos : position
        in uv : texcoord
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in uvOut : texcoord
        return sample(albedoMap, uvOut) * tint
    }
}
)";

inline constexpr const char* kTilemapChunk4TapPhoskiaSource = R"(
material TilemapChunk4Tap {
    texture2d albedoMap
    uniform vec4 atlasTexel
    property tint = vec4(1.0, 1.0, 1.0, 1.0)
    vertex {
        in pos : position
        in uv : texcoord
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in uvOut : texcoord
        let d = atlasTexel.xy * 0.25
        let c = sample(albedoMap, uvOut + vec2(-d.x, -d.y))
              + sample(albedoMap, uvOut + vec2( d.x, -d.y))
              + sample(albedoMap, uvOut + vec2(-d.x,  d.y))
              + sample(albedoMap, uvOut + vec2( d.x,  d.y))
        return c * 0.25 * tint
    }
}
)";

inline constexpr const char* kTilemapChunk9TapPhoskiaSource = R"(
material TilemapChunk9Tap {
    texture2d albedoMap
    uniform vec4 atlasTexel
    property tint = vec4(1.0, 1.0, 1.0, 1.0)
    vertex {
        in pos : position
        in uv : texcoord
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in uvOut : texcoord
        let d = atlasTexel.xy * 0.3333333
        let c = sample(albedoMap, uvOut + vec2(-d.x, -d.y))
              + sample(albedoMap, uvOut + vec2( 0.0, -d.y))
              + sample(albedoMap, uvOut + vec2( d.x, -d.y))
              + sample(albedoMap, uvOut + vec2(-d.x,  0.0))
              + sample(albedoMap, uvOut)
              + sample(albedoMap, uvOut + vec2( d.x,  0.0))
              + sample(albedoMap, uvOut + vec2(-d.x,  d.y))
              + sample(albedoMap, uvOut + vec2( 0.0,  d.y))
              + sample(albedoMap, uvOut + vec2( d.x,  d.y))
        return c * 0.1111111 * tint
    }
}
)";

// Runtime v3 semantic shadows are authored as quarter-cell masks rather
// than baked pixels. Geometry supplies the selected quarters and this
// transparent untextured material supplies the map-wide shadow color.
inline constexpr const char* kTilemapSemanticShadowPhoskiaSource = R"(
material TilemapSemanticShadow {
    property tint = vec4(0.0, 0.0, 0.0, 0.5)
    vertex {
        in pos : position
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        return tint
    }
}
)";

[[nodiscard]] inline constexpr const char* tilemapChunkShaderSource(
    TilemapSamplingQuality quality) noexcept
{
    switch (quality) {
    case TilemapSamplingQuality::Nearest: return kTilemapChunkNearestPhoskiaSource;
    case TilemapSamplingQuality::Tap4: return kTilemapChunk4TapPhoskiaSource;
    case TilemapSamplingQuality::Tap9: return kTilemapChunk9TapPhoskiaSource;
    case TilemapSamplingQuality::Linear: break;
    }
    return kTilemapChunkLinearPhoskiaSource;
}

} // namespace ayt::render
