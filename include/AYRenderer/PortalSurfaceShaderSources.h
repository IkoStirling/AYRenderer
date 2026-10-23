#pragma once
namespace ayt::render {
inline constexpr const char* kPortalSurfaceVaryingSc = R"(
vec4 v_portalClip : TEXCOORD0;
vec3 a_position : POSITION;
)";
inline constexpr const char* kPortalSurfaceVertexSc = R"(
$input a_position
$output v_portalClip
#include <bgfx_shader.sh>
void main() {
    v_portalClip = mul(u_modelViewProj, vec4(a_position, 1.0));
    gl_Position = v_portalClip;
}
)";
inline constexpr const char* kPortalSurfaceFragmentSc = R"(
$input v_portalClip
#include <bgfx_shader.sh>
uniform vec4 portalUv;
SAMPLER2D(portalColor, 0);
void main() {
    vec2 ndc = v_portalClip.xy / v_portalClip.w;
    vec2 uv = ndc * portalUv.xy + portalUv.zw;
    gl_FragColor = texture2D(portalColor, uv);
}
)";
} // namespace ayt::render
