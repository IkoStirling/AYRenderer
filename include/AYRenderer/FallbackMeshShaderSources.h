#pragma once
// Engine-owned legacy fallback sources. Not a recommended gameplay material API.
// Runtime Entity submission and offline production catalogs share exact bytes.
namespace ayt::render {
inline constexpr const char* kSkinnedLitFragmentSc = R"(
$input v_normal, v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    vec3 n = normalize(v_normal);
    vec3 lightDir = normalize(vec3(0.35, -0.85, -0.4));
    float ndotl = max(dot(n, -lightDir), 0.0);
    const float ambient = 0.22;
    const float diffuse = 0.78 * ndotl;
    vec3 baseColor = vec3(0.92, 0.78, 0.55);
    gl_FragColor = vec4(baseColor * (ambient + diffuse), 1.0);
}
)";

inline constexpr const char* kRigidLitVaryingDef = R"(
vec3 v_normal    : NORMAL    = vec3(0.0, 0.0, 1.0);
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec2 a_texcoord0 : TEXCOORD0;
)";

inline constexpr const char* kRigidLitVertexSc = R"(
$input a_position, a_normal, a_texcoord0
$output v_normal, v_texcoord0
#include <bgfx_shader.sh>
void main()
{
    v_texcoord0 = a_texcoord0;
    v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
}
)";
}
