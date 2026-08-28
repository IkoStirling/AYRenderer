#pragma once

namespace ayt::render
{

inline constexpr const char kSsaoCacheKey[] =
    "ssao_v5_8tap_viewdepth_tbn_coverage_fs";

// Full-resolution SSAO estimate. RT2 supplies world position only; geometry
// validity comes exclusively from RT3.a so material AO/model packing can never
// masquerade as coverage. The kernel is oriented around the world-space normal,
// rotated per pixel, and compares view-space Z rather than radial camera range.
inline constexpr const char kSsaoPhoskiaSource[] = R"PHOSKIA(
material SSAO {
    texture2d worldPosition
    texture2d worldNormal
    texture2d geometryCoverage
    uniform vec4 ssaoRadius
    uniform vec4 ssaoBias
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let centerWorld = sample(worldPosition, uv)
        let centerCoverage = step(0.5, sample(geometryCoverage, uv).a)
        let nEnc = sample(worldNormal, uv)
        let Nraw = nEnc.xyz * 2.0 - vec3(1.0, 1.0, 1.0)
        let Nn = Nraw * (1.0 / max(length(Nraw), 0.0001))
        let centerView = viewMatrix * vec4(centerWorld.xyz, 1.0)
        let rad = max(ssaoRadius.x, 0.0001)
        let bias = max(ssaoBias.x, 0.0)

        let helperBlend = step(0.999, abs(Nn.y))
        let helper = mix(vec3(0.0, 1.0, 0.0),
                         vec3(1.0, 0.0, 0.0), helperBlend)
        let tangent0 = normalize(cross(helper, Nn))
        let bitangent0 = normalize(cross(Nn, tangent0))
        let pixel = uv * viewportRect.zw
        let angle = sin(dot(pixel, vec2(12.9898, 78.233))) * 3.14159265
        let ca = cos(angle)
        let sa = sin(angle)
        let basisT = tangent0 * ca + bitangent0 * sa
        let basisB = bitangent0 * ca - tangent0 * sa

        let local0 = normalize(vec3(0.45, 0.15, 0.88))
        let dir0 = basisT * local0.x + basisB * local0.y + Nn * local0.z
        let sampleWorld0 = centerWorld.xyz + dir0 * (rad * 0.25)
        let sampleView0 = viewMatrix * vec4(sampleWorld0, 1.0)
        let p0 = viewProjectionMatrix * vec4(sampleWorld0, 1.0)
        let ndc0 = p0.xyz / max(p0.w, 0.0001)
        let uv0 = vec2(ndc0.x * 0.5 + 0.5, 1.0 - (ndc0.y * 0.5 + 0.5))
        let w0 = sample(worldPosition, uv0)
        let actualView0 = viewMatrix * vec4(w0.xyz, 1.0)
        let to0 = w0.xyz - centerWorld.xyz
        let valid0 = step(0.5, sample(geometryCoverage, uv0).a)
        let bound0 = step(0.0, uv0.x) * step(uv0.x, 1.0) * step(0.0, uv0.y) * step(uv0.y, 1.0)
        let range0 = clamp(1.0 - abs(actualView0.z - centerView.z) / rad, 0.0, 1.0)
        let plane0 = step(bias, dot(to0, Nn))
        let occ0 = step(0.0001, p0.w) * valid0 * bound0 * range0 * plane0 * step(abs(actualView0.z) + bias, abs(sampleView0.z))

        let local1 = normalize(vec3(-0.30, 0.40, 0.86))
        let dir1 = basisT * local1.x + basisB * local1.y + Nn * local1.z
        let sampleWorld1 = centerWorld.xyz + dir1 * (rad * 0.32)
        let sampleView1 = viewMatrix * vec4(sampleWorld1, 1.0)
        let p1 = viewProjectionMatrix * vec4(sampleWorld1, 1.0)
        let ndc1 = p1.xyz / max(p1.w, 0.0001)
        let uv1 = vec2(ndc1.x * 0.5 + 0.5, 1.0 - (ndc1.y * 0.5 + 0.5))
        let w1 = sample(worldPosition, uv1)
        let actualView1 = viewMatrix * vec4(w1.xyz, 1.0)
        let to1 = w1.xyz - centerWorld.xyz
        let valid1 = step(0.5, sample(geometryCoverage, uv1).a)
        let bound1 = step(0.0, uv1.x) * step(uv1.x, 1.0) * step(0.0, uv1.y) * step(uv1.y, 1.0)
        let range1 = clamp(1.0 - abs(actualView1.z - centerView.z) / rad, 0.0, 1.0)
        let plane1 = step(bias, dot(to1, Nn))
        let occ1 = step(0.0001, p1.w) * valid1 * bound1 * range1 * plane1 * step(abs(actualView1.z) + bias, abs(sampleView1.z))

        let local2 = normalize(vec3(0.15, -0.55, 0.82))
        let dir2 = basisT * local2.x + basisB * local2.y + Nn * local2.z
        let sampleWorld2 = centerWorld.xyz + dir2 * (rad * 0.40)
        let sampleView2 = viewMatrix * vec4(sampleWorld2, 1.0)
        let p2 = viewProjectionMatrix * vec4(sampleWorld2, 1.0)
        let ndc2 = p2.xyz / max(p2.w, 0.0001)
        let uv2 = vec2(ndc2.x * 0.5 + 0.5, 1.0 - (ndc2.y * 0.5 + 0.5))
        let w2 = sample(worldPosition, uv2)
        let actualView2 = viewMatrix * vec4(w2.xyz, 1.0)
        let to2 = w2.xyz - centerWorld.xyz
        let valid2 = step(0.5, sample(geometryCoverage, uv2).a)
        let bound2 = step(0.0, uv2.x) * step(uv2.x, 1.0) * step(0.0, uv2.y) * step(uv2.y, 1.0)
        let range2 = clamp(1.0 - abs(actualView2.z - centerView.z) / rad, 0.0, 1.0)
        let plane2 = step(bias, dot(to2, Nn))
        let occ2 = step(0.0001, p2.w) * valid2 * bound2 * range2 * plane2 * step(abs(actualView2.z) + bias, abs(sampleView2.z))

        let local3 = normalize(vec3(-0.60, -0.10, 0.79))
        let dir3 = basisT * local3.x + basisB * local3.y + Nn * local3.z
        let sampleWorld3 = centerWorld.xyz + dir3 * (rad * 0.50)
        let sampleView3 = viewMatrix * vec4(sampleWorld3, 1.0)
        let p3 = viewProjectionMatrix * vec4(sampleWorld3, 1.0)
        let ndc3 = p3.xyz / max(p3.w, 0.0001)
        let uv3 = vec2(ndc3.x * 0.5 + 0.5, 1.0 - (ndc3.y * 0.5 + 0.5))
        let w3 = sample(worldPosition, uv3)
        let actualView3 = viewMatrix * vec4(w3.xyz, 1.0)
        let to3 = w3.xyz - centerWorld.xyz
        let valid3 = step(0.5, sample(geometryCoverage, uv3).a)
        let bound3 = step(0.0, uv3.x) * step(uv3.x, 1.0) * step(0.0, uv3.y) * step(uv3.y, 1.0)
        let range3 = clamp(1.0 - abs(actualView3.z - centerView.z) / rad, 0.0, 1.0)
        let plane3 = step(bias, dot(to3, Nn))
        let occ3 = step(0.0001, p3.w) * valid3 * bound3 * range3 * plane3 * step(abs(actualView3.z) + bias, abs(sampleView3.z))

        let local4 = normalize(vec3(0.70, 0.45, 0.56))
        let dir4 = basisT * local4.x + basisB * local4.y + Nn * local4.z
        let sampleWorld4 = centerWorld.xyz + dir4 * (rad * 0.62)
        let sampleView4 = viewMatrix * vec4(sampleWorld4, 1.0)
        let p4 = viewProjectionMatrix * vec4(sampleWorld4, 1.0)
        let ndc4 = p4.xyz / max(p4.w, 0.0001)
        let uv4 = vec2(ndc4.x * 0.5 + 0.5, 1.0 - (ndc4.y * 0.5 + 0.5))
        let w4 = sample(worldPosition, uv4)
        let actualView4 = viewMatrix * vec4(w4.xyz, 1.0)
        let to4 = w4.xyz - centerWorld.xyz
        let valid4 = step(0.5, sample(geometryCoverage, uv4).a)
        let bound4 = step(0.0, uv4.x) * step(uv4.x, 1.0) * step(0.0, uv4.y) * step(uv4.y, 1.0)
        let range4 = clamp(1.0 - abs(actualView4.z - centerView.z) / rad, 0.0, 1.0)
        let plane4 = step(bias, dot(to4, Nn))
        let occ4 = step(0.0001, p4.w) * valid4 * bound4 * range4 * plane4 * step(abs(actualView4.z) + bias, abs(sampleView4.z))

        let local5 = normalize(vec3(-0.40, 0.75, 0.53))
        let dir5 = basisT * local5.x + basisB * local5.y + Nn * local5.z
        let sampleWorld5 = centerWorld.xyz + dir5 * (rad * 0.74)
        let sampleView5 = viewMatrix * vec4(sampleWorld5, 1.0)
        let p5 = viewProjectionMatrix * vec4(sampleWorld5, 1.0)
        let ndc5 = p5.xyz / max(p5.w, 0.0001)
        let uv5 = vec2(ndc5.x * 0.5 + 0.5, 1.0 - (ndc5.y * 0.5 + 0.5))
        let w5 = sample(worldPosition, uv5)
        let actualView5 = viewMatrix * vec4(w5.xyz, 1.0)
        let to5 = w5.xyz - centerWorld.xyz
        let valid5 = step(0.5, sample(geometryCoverage, uv5).a)
        let bound5 = step(0.0, uv5.x) * step(uv5.x, 1.0) * step(0.0, uv5.y) * step(uv5.y, 1.0)
        let range5 = clamp(1.0 - abs(actualView5.z - centerView.z) / rad, 0.0, 1.0)
        let plane5 = step(bias, dot(to5, Nn))
        let occ5 = step(0.0001, p5.w) * valid5 * bound5 * range5 * plane5 * step(abs(actualView5.z) + bias, abs(sampleView5.z))

        let local6 = normalize(vec3(0.65, -0.65, 0.39))
        let dir6 = basisT * local6.x + basisB * local6.y + Nn * local6.z
        let sampleWorld6 = centerWorld.xyz + dir6 * (rad * 0.87)
        let sampleView6 = viewMatrix * vec4(sampleWorld6, 1.0)
        let p6 = viewProjectionMatrix * vec4(sampleWorld6, 1.0)
        let ndc6 = p6.xyz / max(p6.w, 0.0001)
        let uv6 = vec2(ndc6.x * 0.5 + 0.5, 1.0 - (ndc6.y * 0.5 + 0.5))
        let w6 = sample(worldPosition, uv6)
        let actualView6 = viewMatrix * vec4(w6.xyz, 1.0)
        let to6 = w6.xyz - centerWorld.xyz
        let valid6 = step(0.5, sample(geometryCoverage, uv6).a)
        let bound6 = step(0.0, uv6.x) * step(uv6.x, 1.0) * step(0.0, uv6.y) * step(uv6.y, 1.0)
        let range6 = clamp(1.0 - abs(actualView6.z - centerView.z) / rad, 0.0, 1.0)
        let plane6 = step(bias, dot(to6, Nn))
        let occ6 = step(0.0001, p6.w) * valid6 * bound6 * range6 * plane6 * step(abs(actualView6.z) + bias, abs(sampleView6.z))

        let local7 = normalize(vec3(-0.72, -0.50, 0.48))
        let dir7 = basisT * local7.x + basisB * local7.y + Nn * local7.z
        let sampleWorld7 = centerWorld.xyz + dir7 * rad
        let sampleView7 = viewMatrix * vec4(sampleWorld7, 1.0)
        let p7 = viewProjectionMatrix * vec4(sampleWorld7, 1.0)
        let ndc7 = p7.xyz / max(p7.w, 0.0001)
        let uv7 = vec2(ndc7.x * 0.5 + 0.5, 1.0 - (ndc7.y * 0.5 + 0.5))
        let w7 = sample(worldPosition, uv7)
        let actualView7 = viewMatrix * vec4(w7.xyz, 1.0)
        let to7 = w7.xyz - centerWorld.xyz
        let valid7 = step(0.5, sample(geometryCoverage, uv7).a)
        let bound7 = step(0.0, uv7.x) * step(uv7.x, 1.0) * step(0.0, uv7.y) * step(uv7.y, 1.0)
        let range7 = clamp(1.0 - abs(actualView7.z - centerView.z) / rad, 0.0, 1.0)
        let plane7 = step(bias, dot(to7, Nn))
        let occ7 = step(0.0001, p7.w) * valid7 * bound7 * range7 * plane7 * step(abs(actualView7.z) + bias, abs(sampleView7.z))

        let occSum = occ0 + occ1 + occ2 + occ3 + occ4 + occ5 + occ6 + occ7
        let occFraction = clamp(occSum * 0.125, 0.0, 1.0)
        let aoOcclusion = clamp((occFraction - 0.1) * 1.111111, 0.0, 1.0)
        return vec4(aoOcclusion * centerCoverage, 0.0, 0.0, centerCoverage)
    }
}
)PHOSKIA";

} // namespace ayt::render
