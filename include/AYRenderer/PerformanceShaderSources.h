#pragma once
#include <string>
namespace ayt::render {
inline constexpr const char* kPerformancePatternSource=R"(
fn performanceShadowOffset(index:int, count:float):vec2 {
 let angle=float(index)*6.283185307/count
 let radius=0.35+0.55*(float(index)-floor(float(index)*0.5)*2.0)
 return vec2(cos(angle),sin(angle))*radius
}
fn performanceShadowDistance(encoded:float, area:vec4):float {
 return area.y*area.z/max(area.z-(encoded*2.0-1.0)*(area.z-area.y),0.00000001)
}
fn performancePenumbraRadius(radius:float, receiver:float, blocker:float):float {
 return radius*max(receiver-blocker,0.0)/max(blocker,0.00001)
}
fn performanceIesUv(delta:vec3, axis:vec3, right:vec3, up:vec3):vec2 {
 let d=delta/max(length(delta),0.00001)
 let theta=acos(clamp(dot(d,axis),-1.0,1.0))*0.318309886
 let phi=atan2(dot(d,up),dot(d,right))*0.159154943
 if(phi<0.0){phi=phi+1.0}
 return vec2((phi*256.0+0.5)/257.0,(theta*128.0+0.5)/129.0)
}
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
fn performanceLobeDirection(axis:vec3, right:vec3, up:vec3, lens:vec4, index:float):vec3 {
 if(lens.y<1.5 || lens.z<0.000001){return axis}
 let angle=lens.w+index*6.283185307/lens.y
 return axis*cos(lens.z)+(right*cos(angle)+up*sin(angle))*sin(lens.z)
}
fn performanceFocusBlur(distance:float, cone:vec4, iris:float, lens:vec4):float {
 if(lens.x<=0.0){return 0.0}
 return min(2.0,cone.w*iris*abs(1.0-max(distance,0.0)/lens.x)/max(cone.w+max(distance,0.0)*cone.z,0.000000001))
}
fn performancePrismInterval(origin:vec3, ray:vec3, position:vec3, axis:vec3, right:vec3, up:vec3, cone:vec4, reach:float, segment:vec2, lens:vec4):vec2 {
 if(lens.y<1.5){return performanceBeamInterval(origin,ray,position,axis,cone,reach,segment)}
 let begin=segment.y;let end=segment.x
 for(lobe in range(int(lens.y),8)){
  let d=performanceLobeDirection(axis,right,up,lens,float(lobe))
  let hit=performanceBeamInterval(origin,ray,position,d,cone,reach,segment)
  if(hit.y>hit.x){begin=min(begin,hit.x);end=max(end,hit.y)}
 }
 if(end<=begin){return vec2(0.0)}
 return vec2(begin,end)
}
fn performanceGoboUv(p:vec2, settings:vec4):vec2 {
 let c=cos(settings.y);let s=sin(settings.y)
 return vec2(p.x*c-p.y*s,p.x*s+p.y*c)*settings.z*0.5+vec2(0.5)
}
fn performancePattern(p:vec2, settings:vec4, optics:vec4, textureMask:float):float {
 let iris=1.0
 if(optics.y<0.999){iris=1.0-smoothstep(optics.y-0.02,optics.y+0.02,length(p))}
 if(settings.x<0.5){return iris}
 if(settings.x>4.5){return textureMask*iris}
 let c=cos(settings.y);let s=sin(settings.y)
 let uv=vec2(p.x*c-p.y*s,p.x*s+p.y*c)*settings.z
 let result=1.0
 if(settings.x>0.5 && settings.x<1.5){result=smoothstep(-0.15-optics.x*0.5,0.15+optics.x*0.5,sin(uv.x*25.132741))}
 if(settings.x>1.5 && settings.x<2.5){result=smoothstep(0.0-optics.x*0.5,0.25+optics.x*0.5,sin(uv.x*18.0)*sin(uv.y*18.0))}
 if(settings.x>2.5 && settings.x<3.5){result=1.0-smoothstep(0.07-optics.x*0.2,0.12+optics.x*0.2,abs(length(uv)-0.55))}
 if(settings.x>3.5 && settings.x<4.5){
  let ray=0.0
  ray=max(ray,(1.0-smoothstep(0.03-optics.x*0.2,0.08+optics.x*0.2,abs(dot(uv,vec2(-sin(0.000000000),cos(0.000000000))))))*step(0.0,dot(uv,vec2(cos(0.000000000),sin(0.000000000)))))
  ray=max(ray,(1.0-smoothstep(0.03-optics.x*0.2,0.08+optics.x*0.2,abs(dot(uv,vec2(-sin(1.256637060),cos(1.256637060))))))*step(0.0,dot(uv,vec2(cos(1.256637060),sin(1.256637060)))))
  ray=max(ray,(1.0-smoothstep(0.03-optics.x*0.2,0.08+optics.x*0.2,abs(dot(uv,vec2(-sin(2.513274120),cos(2.513274120))))))*step(0.0,dot(uv,vec2(cos(2.513274120),sin(2.513274120)))))
  ray=max(ray,(1.0-smoothstep(0.03-optics.x*0.2,0.08+optics.x*0.2,abs(dot(uv,vec2(-sin(3.769911180),cos(3.769911180))))))*step(0.0,dot(uv,vec2(cos(3.769911180),sin(3.769911180)))))
  ray=max(ray,(1.0-smoothstep(0.03-optics.x*0.2,0.08+optics.x*0.2,abs(dot(uv,vec2(-sin(5.026548240),cos(5.026548240))))))*step(0.0,dot(uv,vec2(cos(5.026548240),sin(5.026548240)))))
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
 texture2d contactPosition
 texture2d contactCoverage
 texture2d shadowAtlas
 texture2d patternGobo
 texture2d iesAtlas
 uniform vec4 performancePosition
 uniform vec4 performanceDirection
 uniform vec4 performanceColor
 uniform vec4 performanceCone
 uniform vec4 performanceRight
 uniform vec4 performanceUp
 uniform vec4 performancePatternSettings
 uniform vec4 performanceLens
 uniform vec4 performanceShadowArea
 uniform vec4 performanceShadowAreaOptions
 uniform vec4 performanceIesSettings
 uniform vec4 performancePatternOptics
 uniform vec4 performanceEye
 uniform vec4 performanceContact
 uniform mat4 performanceViewProjection
 uniform mat4 performanceView
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
  let response=1.0
  if(performanceDirection.w<0.5){
  let optical=0.0
  for(lobe in range(int(performanceLens.y),8)){
   let beamAxis=performanceLobeDirection(performanceDirection.xyz,performanceRight.xyz,performanceUp.xyz,performanceLens,float(lobe))
   let reference=vec3(0.0,1.0,0.0)
   if(abs(beamAxis.y)>0.99){reference=vec3(0.0,0.0,1.0)}
   let beamRight=normalize(cross(reference,beamAxis));let beamUp=normalize(cross(beamAxis,beamRight))
   let axial=dot(delta,beamAxis)
   let projected=vec2(dot(delta,beamRight),dot(delta,beamUp))/max(performanceCone.w+axial*performanceCone.z,0.00001)
   let defocus=performanceFocusBlur(axial,performanceCone,performancePatternOptics.y,performanceLens)
   let localOptics=performancePatternOptics
   let textureMask=1.0
   if(performancePatternSettings.x>4.5){
    let goboUv=performanceGoboUv(projected,performancePatternSettings)
    let inside=step(0.0,goboUv.x)*step(goboUv.x,1.0)*step(0.0,goboUv.y)*step(goboUv.y,1.0)
    let texel=sampleLod(patternGobo,goboUv,0.0)
    if(performancePatternOptics.x>0.01 || defocus>0.0001){
     let blur=performancePatternOptics.x*0.025+defocus*performancePatternSettings.z*0.5
     texel=(texel+sampleLod(patternGobo,goboUv+vec2(blur,0.0),0.0)
           +sampleLod(patternGobo,goboUv-vec2(blur,0.0),0.0)
           +sampleLod(patternGobo,goboUv+vec2(0.0,blur),0.0)
           +sampleLod(patternGobo,goboUv-vec2(0.0,blur),0.0))*0.2
    }
    textureMask=dot(texel.rgb,vec3(0.2126,0.7152,0.0722))*texel.a*inside
   }
   let pattern=performancePattern(projected,performancePatternSettings,localOptics,textureMask)
   if(defocus>0.0001 && performancePatternSettings.x>0.5 && performancePatternSettings.x<4.5){
    let pupil=defocus
    pattern=(pattern+performancePattern(projected+vec2(pupil,0.0),performancePatternSettings,localOptics,1.0)
                    +performancePattern(projected-vec2(pupil,0.0),performancePatternSettings,localOptics,1.0)
                    +performancePattern(projected+vec2(0.0,pupil),performancePatternSettings,localOptics,1.0)
                    +performancePattern(projected-vec2(0.0,pupil),performancePatternSettings,localOptics,1.0))*0.2
   }
   let beam=performanceBeamProfile(delta,beamAxis,performanceCone,performanceRight.w)
   let distribution=1.0
   if(performanceIesSettings.x>0.5){distribution=sampleLod(iesAtlas,performanceIesUv(delta,beamAxis,beamRight,beamUp),0.0).r}
   optical=optical+beam*pattern*distribution/performanceLens.y
  }
  response=optical
  }
  let falloff=clamp(1.0-distance/performancePosition.w,0.0,1.0)
  let attenuation=falloff*falloff/max(distance*distance,max(0.01,performanceCone.w*performanceCone.w))*response
  let visibility=1.0
  if(performanceDirection.w>0.5 && performanceContact.x>0.0 && performanceContact.w>0.0){
   let traceLength=min(distance,performanceContact.x)
   for(contactTap in range(8)){
    let fraction=(float(contactTap)+0.5)/8.0
    let rayPoint=position.xyz+N*performanceContact.z+L*(traceLength*fraction)
    let clipContact=performanceViewProjection*vec4(rayPoint,1.0)
    if(clipContact.w>0.00001){
     let contactUv=vec2(clipContact.x/clipContact.w*0.5+0.5,0.5-clipContact.y/clipContact.w*0.5)
     if(contactUv.x>0.0 && contactUv.x<1.0 && contactUv.y>0.0 && contactUv.y<1.0){
      let seen=sampleLod(contactPosition,contactUv,0.0).xyz
      let seenCoverage=sampleLod(contactCoverage,contactUv,0.0).a
      let gap=(performanceView*vec4(rayPoint-seen,0.0)).z
      if(seenCoverage>0.999 && gap>performanceContact.z && gap<performanceContact.y){visibility=1.0-performanceContact.w}
     }
    }
   }
  }
  if(performanceShadowSettings.x>0.5){
   let clip=performanceShadow*vec4(position.xyz,1.0)
   let q=clip.xyz/max(clip.w,0.00001)
   let localUv=vec2(q.x*0.5+0.5,0.5-q.y*0.5)
   let ref=q.z*0.5+0.5
   if(clip.w>0.0 && localUv.x>=0.0 && localUv.x<=1.0 && localUv.y>=0.0 && localUv.y<=1.0 && ref>=0.0 && ref<=1.0){
    let shadowUv=mix(performanceShadowRect.xy,performanceShadowRect.zw,localUv)
    if(performanceShadowArea.x>0.0){
     let tile=performanceShadowRect.zw-performanceShadowRect.xy
     let edge=vec2(performanceShadowAreaOptions.y*0.5)
     let receiver=max(clip.w,performanceShadowArea.y)
     let searchUv=performanceShadowArea.x*max(receiver-performanceShadowArea.y,0.0)/max(2.0*performanceShadowArea.w*performanceShadowArea.y*receiver,0.00001)
     let search=vec2(min(performanceShadowAreaOptions.w*performanceShadowAreaOptions.y,tile.x*searchUv),min(performanceShadowAreaOptions.w*performanceShadowAreaOptions.y,tile.y*searchUv))
     let blockerSum=0.0;let blockers=0.0
     let centerDepth=sampleLod(shadowAtlas,shadowUv,0.0).r
     let centerDistance=performanceShadowDistance(centerDepth,performanceShadowArea)
     if(centerDepth<0.999999 && centerDistance+performanceShadowAreaOptions.z<receiver){blockerSum=centerDistance;blockers=1.0}
     for(blockerTap in range(int(performanceShadowAreaOptions.x),16)){
      let tap=clamp(shadowUv+performanceShadowOffset(blockerTap,performanceShadowAreaOptions.x)*search,performanceShadowRect.xy+edge,performanceShadowRect.zw-edge)
      let depth=sampleLod(shadowAtlas,tap,0.0).r
      let distance=performanceShadowDistance(depth,performanceShadowArea)
      if(depth<0.999999 && distance+performanceShadowAreaOptions.z<receiver){blockerSum=blockerSum+distance;blockers=blockers+1.0}
     }
     visibility=1.0
     if(blockers>0.5){
      let blocker=blockerSum/blockers
      let worldRadius=performancePenumbraRadius(performanceShadowArea.x,receiver,blocker)
      let radiusUv=worldRadius/max(2.0*performanceShadowArea.w*receiver,0.00001)
      let radius=vec2(max(performanceShadowSettings.z*performanceShadowAreaOptions.y,min(performanceShadowAreaOptions.w*performanceShadowAreaOptions.y,tile.x*radiusUv)),max(performanceShadowSettings.z*performanceShadowAreaOptions.y,min(performanceShadowAreaOptions.w*performanceShadowAreaOptions.y,tile.y*radiusUv)))
      let soft=0.0
      for(filterTap in range(int(performanceShadowAreaOptions.x),16)){
       let tap=clamp(shadowUv+performanceShadowOffset(filterTap,performanceShadowAreaOptions.x)*radius,performanceShadowRect.xy+edge,performanceShadowRect.zw-edge)
       let depth=sampleLod(shadowAtlas,tap,0.0).r
       let distance=performanceShadowDistance(depth,performanceShadowArea)
       soft=soft+max(step(receiver-performanceShadowAreaOptions.z,distance),step(0.999999,depth))
      }
      visibility=soft/performanceShadowAreaOptions.x
     }
    } else if(performanceShadowSettings.z<0.001){
     let depth=sampleLod(shadowAtlas,shadowUv,0.0).r
     visibility=max(1.0-step(depth+performanceShadowSettings.y,ref),step(0.999,depth))
    } else {
     let radius=performanceShadowSettings.z*performanceShadowSettings.w
     let edge=vec2(performanceShadowSettings.w*0.5)
     let soft=0.0
     for(j in range(4)) {
      let ox=-1.0;let oy=-1.0
      if(j==1 || j==3){ox=1.0}
      if(j>=2){oy=1.0}
      let tap=clamp(shadowUv+vec2(ox,oy)*radius,performanceShadowRect.xy+edge,performanceShadowRect.zw-edge)
      let depth=sampleLod(shadowAtlas,tap,0.0).r
      soft=soft+max(1.0-step(depth+performanceShadowSettings.y,ref),step(0.999,depth))
     }
     visibility=soft*0.25
    }
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
