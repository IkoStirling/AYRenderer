#pragma once
#include <string>
namespace ayt::render {
inline constexpr const char* kPerformancePatternSource=R"(
fn performancePattern(p:vec2, settings:vec4):float {
 let c=cos(settings.y);let s=sin(settings.y)
 let uv=vec2(p.x*c-p.y*s,p.x*s+p.y*c)*settings.z
 let result=1.0
 if(settings.x>0.5 && settings.x<1.5){result=smoothstep(-0.15,0.15,sin(uv.x*25.132741))}
 if(settings.x>1.5 && settings.x<2.5){result=smoothstep(0.0,0.25,sin(uv.x*18.0)*sin(uv.y*18.0))}
 if(settings.x>2.5 && settings.x<3.5){result=1.0-smoothstep(0.07,0.12,abs(length(uv)-0.55))}
 if(settings.x>3.5){
  let ray=0.0
  for(j in range(5)) {
   let a=float(j)*1.25663706
   let along=dot(uv,vec2(cos(a),sin(a)))
   let across=abs(dot(uv,vec2(-sin(a),cos(a))))
   ray=max(ray,(1.0-smoothstep(0.03,0.08,across))*step(0.0,along))
  }
  result=ray
 }
 return result
}
)";
inline const std::string kPerformanceSurfaceSource=std::string(kPerformancePatternSource)+R"(
material PerformanceSurface {
 texture2d surfaceAlbedo
 texture2d surfaceNormal
 texture2d surfacePosition
 texture2d surfaceCoverage
 texture2d shadowAtlas
 uniform vec4 performancePosition
 uniform vec4 performanceDirection
 uniform vec4 performanceColor
 uniform vec4 performanceCone
 uniform vec4 performanceRight
 uniform vec4 performanceUp
 uniform vec4 performancePatternSettings
 uniform vec4 performanceEye
 uniform mat4 performanceShadow
 uniform vec4 performanceShadowRect
 uniform vec4 performanceShadowSettings
 vertex {in pos:position;out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5);return vec4(pos.xy,0.0,1.0)}
 fragment {
  in vUv:texcoord
  let uv=vec2(vUv.x,1.0-vUv.y)
  let position=sample(surfacePosition,uv)
  let coverage=sample(surfaceCoverage,uv).a
  let albedo=sample(surfaceAlbedo,uv)
  let normalRoughness=sample(surfaceNormal,uv)
  let rawN=normalRoughness.xyz*2.0-vec3(1.0)
  let N=rawN/max(length(rawN),0.00001)
  let delta=position.xyz-performancePosition.xyz
  let distance=length(delta)
  let outward=delta/max(distance,0.00001)
  let L=outward*-1.0
  let axial=dot(delta,performanceDirection.xyz)
  let projected=vec2(dot(delta,performanceRight.xyz),dot(delta,performanceUp.xyz))/max(axial*performanceCone.z,0.00001)
  let pattern=performancePattern(projected,performancePatternSettings)
  let falloff=clamp(1.0-distance/performancePosition.w,0.0,1.0)
  let attenuation=falloff*falloff/max(distance*distance,0.01)*smoothstep(performanceCone.y,performanceCone.x,dot(outward,performanceDirection.xyz))*pattern
  let visibility=1.0
  if(performanceShadowSettings.x>0.5){
   let clip=performanceShadow*vec4(position.xyz,1.0)
   let q=clip.xyz/max(clip.w,0.00001)
   let localUv=vec2(q.x*0.5+0.5,0.5-q.y*0.5)
   let ref=q.z*0.5+0.5
   if(clip.w>0.0 && localUv.x>=0.0 && localUv.x<=1.0 && localUv.y>=0.0 && localUv.y<=1.0 && ref>=0.0 && ref<=1.0){
    let depth=sampleLod(shadowAtlas,mix(performanceShadowRect.xy,performanceShadowRect.zw,localUv),0.0).r
    visibility=max(1.0-step(depth+performanceShadowSettings.y,ref),step(0.999,depth))
   }
  }
  let toEye=performanceEye.xyz-position.xyz
  let V=toEye/max(length(toEye),0.00001)
  let rawH=V+L
  let H=rawH/max(length(rawH),0.00001)
  let nl=max(dot(N,L),0.0);let nv=max(dot(N,V),0.0001);let nh=max(dot(N,H),0.0)
  let metallic=clamp(albedo.a,0.0,1.0);let roughness=clamp(normalRoughness.a,0.045,1.0)
  let f0=mix(vec3(0.04),albedo.rgb,metallic)
  let F=f0+(vec3(1.0)-f0)*pow(1.0-max(dot(V,H),0.0),5.0)
  let alpha=roughness*roughness;let alpha2=alpha*alpha
  let denominator=nh*nh*(alpha2-1.0)+1.0
  let D=alpha2/max(3.14159265*denominator*denominator,0.000001)
  let k=(roughness+1.0)*(roughness+1.0)/8.0
  let G=(nv/(nv*(1.0-k)+k))*(nl/max(nl*(1.0-k)+k,0.0001))
  let diffuse=(vec3(1.0)-F)*albedo.rgb*(1.0-metallic)*0.318309886
  let specular=F*(D*G/max(4.0*nv*nl,0.001))
  let standard=1.0-step(0.5,floor(position.a*0.5+0.0001))
  return vec4((diffuse+specular)*performanceColor.rgb*nl*attenuation*visibility*standard*step(0.5,coverage),0.0)
 }
}
)";
} // namespace ayt::render
