#pragma once

namespace ayt::render::detail
{

// Backing program for a Material2D instance. WorldLit2D GBuffer rendering uses
// its own geometry program, but the material program declares the canonical
// texture/property names so RenderResourceManager can retain their bindings.
// Keeping this source in Phoskia also gives accidental non-GBuffer submission
// a safe albedo-only result instead of introducing a raw .sc exception.
inline constexpr const char* kWorldLit2DMaterialPhoskiaSource = R"(
material WorldLit2DMaterial {
    texture2d albedoMap
    texture2d normalMap
    texture2d roughnessMap
    texture2d emissiveMap
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)
    property metallic = vec4(0.0, 0.0, 0.0, 0.0)
    property roughness = vec4(0.75, 0.0, 0.0, 0.0)
    property ao = vec4(1.0, 0.0, 0.0, 0.0)
    property emissive = vec4(0.0, 0.0, 0.0, 0.0)
    property normalYSign = vec4(1.0, 0.0, 0.0, 0.0)
    vertex {
        in pos : position
        in uv : texcoord
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in uvOut : texcoord
        let albedo = sample(albedoMap, uvOut) * baseColor
        return albedo
    }
}
)";

} // namespace ayt::render::detail
