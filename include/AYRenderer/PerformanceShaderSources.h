#pragma once
#include <string>
namespace ayt::render {
inline constexpr const char* kPerformancePatternSource=R"(
// cone = (cos(core half-angle), cos(field half-angle), tan(field), aperture).
fn performanceBeamProfile(delta:vec3, axis:vec3, cone:vec4, coreSlope:float):float {
 let axial=dot(delta,axis)
 let distance=length(delta)
 let profile=smoothstep(cone.y,max(cone.x,cone.y+0.000001),axial/max(distance,0.00001))
 if(cone.w>0.0){
  let radial=length(delta-axis*axial)
  let core=cone.w+max(axial,0.0)*coreSlope
  let field=cone.w+max(axial,0.0)*cone.z
  profile=(1.0-smoothstep(core,max(field,core+0.000001),radial))*step(0.0,axial)
 }
 return profile
}
// Intersect an observer ray with the actual forward expanding beam. The AABB
// is only a broad phase: sampling its empty space creates detached bright slices.
fn performanceBeamInterval(origin:vec3, ray:vec3, position:vec3, axis:vec3, cone:vec4, reach:float, segment:vec2):vec2 {
 let o=origin-position
 let oz=dot(o,axis);let dz=dot(ray,axis)
 let begin=segment.x;let end=segment.y
 if(abs(dz)<0.000001){
  if(oz<0.0 || oz>reach){return vec2(0.0)}
 } else {
  let z0=(0.0-oz)/dz;let z1=(reach-oz)/dz
  begin=max(begin,min(z0,z1));end=min(end,max(z0,z1))
 }
 let op=o-axis*oz;let dp=ray-axis*dz
 let radius=cone.w+oz*cone.z;let slope=dz*cone.z
 let a=dot(dp,dp)-slope*slope
 let b=2.0*(dot(op,dp)-radius*slope)
 let c=dot(op,op)-radius*radius
 if(abs(a)<0.000001){
  if(abs(b)<0.000001){if(c>0.0){return vec2(0.0)}}
  else if(b>0.0){end=min(end,0.0-c/b)}
  else {begin=max(begin,0.0-c/b)}
 } else {
  let discriminant=b*b-4.0*a*c
  if(discriminant<0.0){if(a>0.0){return vec2(0.0)}}
  else {
   // Stable quadratic roots: avoid subtracting almost equal values.
   let signB=1.0
   if(b<0.0){signB=-1.0}
   let q=-0.5*(b+signB*sqrt(max(discriminant,0.0)))
   let r0=0.0-b/(2.0*a);let r1=r0
   if(abs(q)>0.0000001){r0=q/a;r1=c/q}
   let low=min(r0,r1);let high=max(r0,r1)
   if(a>0.0){begin=max(begin,low);end=min(end,high)}
   else if(begin<high && end>low){
    if(begin<low){end=min(end,low)}else{begin=max(begin,high)}
   }
  }
 }
 if(end<=begin){return vec2(0.0)}
 return vec2(begin,end)
}
fn performancePattern(p:vec2, settings:vec4, optics:vec4):float {
 let iris=1.0
 if(optics.y<0.999){iris=1.0-smoothstep(optics.y-0.02,optics.y+0.02,length(p))}
 if(settings.x<0.5){return iris}
 let c=cos(settings.y);let s=sin(settings.y)
 let uv=vec2(p.x*c-p.y*s,p.x*s+p.y*c)*settings.z
 let result=1.0
 if(settings.x>0.5 && settings.x<1.5){result=smoothstep(-0.15-optics.x*0.5,0.15+optics.x*0.5,sin(uv.x*25.132741))}
 if(settings.x>1.5 && settings.x<2.5){result=smoothstep(0.0-optics.x*0.5,0.25+optics.x*0.5,sin(uv.x*18.0)*sin(uv.y*18.0))}
 if(settings.x>2.5 && settings.x<3.5){result=1.0-smoothstep(0.07-optics.x*0.2,0.12+optics.x*0.2,abs(length(uv)-0.55))}
 if(settings.x>3.5){
  let ray=0.0
  for(j in range(5)) {
   let a=float(j)*1.25663706
   let along=dot(uv,vec2(cos(a),sin(a)))
   let across=abs(dot(uv,vec2(-sin(a),cos(a))))
   ray=max(ray,(1.0-smoothstep(0.03-optics.x*0.2,0.08+optics.x*0.2,across))*step(0.0,along))
  }
  result=ray
 }
 return result*iris
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
 uniform vec4 performancePatternOptics
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
  let projected=vec2(dot(delta,performanceRight.xyz),dot(delta,performanceUp.xyz))/max(performanceCone.w+axial*performanceCone.z,0.00001)
  let pattern=performancePattern(projected,performancePatternSettings,performancePatternOptics)
  let falloff=clamp(1.0-distance/performancePosition.w,0.0,1.0)
  let attenuation=falloff*falloff/max(distance*distance,max(0.01,performanceCone.w*performanceCone.w))*performanceBeamProfile(delta,performanceDirection.xyz,performanceCone,performanceRight.w)*pattern
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
