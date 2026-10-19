#pragma once
#include "AYRenderer/PerformanceShaderSources.h"
namespace ayt::render {
inline constexpr char kVolumetricCacheKey[]="volumetric_v15_froxel_coefficients";
inline const std::string kVolumetricPhoskiaOwned=std::string(kPerformancePatternSource)+R"PHOSKIA(
material Volumetric {
 texture2d sceneColor
 texture2d worldPosition
 texture2d geometryCoverage
 texture2d shadowAtlas
 texture2d patternGobo
 texture2d iesAtlas
 uniform mat4 fogInverseViewProjection
 uniform mat4 fogView
 uniform mat4 fogShadow0
 uniform mat4 fogShadow1
 uniform mat4 fogShadow2
 uniform vec4 fogShadowRect0
 uniform vec4 fogShadowRect1
 uniform vec4 fogShadowRect2
 uniform vec4 fogShadowParams
 uniform vec4 fogCamera
 uniform vec4 fogMedium
 uniform vec4 fogIntegration
 uniform vec4 fogOptions
 uniform vec4 fogCenter
 uniform vec4 fogExtent
 uniform vec4 fogTint
 uniform vec4 fogNoise
 uniform vec4 fogLightPosition
 uniform vec4 fogLightDirection
 uniform vec4 fogLightColor
 uniform vec4 fogSpot
 uniform vec4 fogPatternSettings
 uniform vec4 fogLens
 uniform vec4 fogShadowArea
 uniform vec4 fogShadowAreaOptions
 uniform vec4 fogIesSettings
 uniform vec4 fogPatternOptics
 uniform vec4 fogPatternRight
 uniform vec4 fogPatternUp
 uniform vec4 fogLightBoundsCenter
 uniform vec4 fogLightBoundsExtent
 vertex { in pos:position; out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5); return vec4(pos.xy,0.0,1.0) }
 fragment {
  in vUv:texcoord
  let uv=vec2(vUv.x,1.0-vUv.y)
  let gridSlice=0.0
  if(fogIntegration.y>0.5){
   let tileX=floor(uv.x*fogIntegration.y);let tileY=floor(uv.y*fogIntegration.z)
   gridSlice=tileY*fogIntegration.y+tileX
   uv=vec2(uv.x*fogIntegration.y-tileX,uv.y*fogIntegration.z-tileY)
  }
  let raw=vec4(0.0);let wp=vec4(0.0);let coverage=0.0
  if(fogIntegration.y<0.5){raw=sample(sceneColor,uv);wp=sample(worldPosition,uv);coverage=sample(geometryCoverage,uv).a}
  let farH=fogInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,1.0,1.0)
  let nearH=fogInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,fogCenter.w,1.0)
  let origin=mix(fogCamera.xyz,nearH.xyz/nearH.w,fogCamera.w)
  let endPoint=mix(farH.xyz/farH.w,wp.xyz,step(0.5,coverage))
  let delta=endPoint-origin
  let distance=min(length(delta),fogMedium.w)
  let direction=delta/max(length(delta),0.00001)
  let begin=0.0
  let end=distance
  if(fogIntegration.y>0.5){
   begin=exp(log(1.0+fogMedium.w)*gridSlice/fogIntegration.w)-1.0
   end=min(end,exp(log(1.0+fogMedium.w)*(gridSlice+1.0)/fogIntegration.w)-1.0)
  }
  if (fogOptions.z>0.5) {
   let safeDirection=mix(vec3(0.00001),direction,step(vec3(0.00001),abs(direction)))
   let a=(fogCenter.xyz-fogExtent.xyz-origin)/safeDirection
   let b=(fogCenter.xyz+fogExtent.xyz-origin)/safeDirection
   begin=max(0.0,max(max(min(a.x,b.x),min(a.y,b.y)),min(a.z,b.z)))
   end=min(distance,min(min(max(a.x,b.x),max(a.y,b.y)),max(a.z,b.z)))
  }
  let mediumBegin=begin
  if(fogOptions.w>1.5&&fogIntegration.y<0.5){
   let safeDirection=mix(vec3(0.00001),direction,step(vec3(0.00001),abs(direction)))
   let a=(fogLightBoundsCenter.xyz-fogLightBoundsExtent.xyz-origin)/safeDirection
   let b=(fogLightBoundsCenter.xyz+fogLightBoundsExtent.xyz-origin)/safeDirection
   begin=max(begin,max(max(min(a.x,b.x),min(a.y,b.y)),min(a.z,b.z)))
   end=min(end,min(min(max(a.x,b.x),max(a.y,b.y)),max(a.z,b.z)))
   if(fogLightPosition.w>1.5){
    let segment=performancePrismInterval(origin,direction,fogLightPosition.xyz,fogLightDirection.xyz,fogPatternRight.xyz,fogPatternUp.xyz,fogSpot,fogLightDirection.w,vec2(begin,end),fogLens)
    begin=segment.x;end=segment.y
   }
  }
  // Scatter-only pixels outside the actual beam contribute exactly zero.
  // Do not execute a 48/64-step loop over an empty AABB/cone intersection.
  if(fogOptions.w>1.5 && end<=begin){return vec4(0.0)}
  let sampleCount=fogOptions.x
  // A four-point quadrature is sufficient for the smooth box extinction;
  // only scattering needs the user's full per-beam step budget.
  if(fogOptions.z>0.5 && fogOptions.w>0.5 && fogOptions.w<1.5){sampleCount=4.0}
  if(fogOptions.w>1.5 && fogLightPosition.w>1.5 && fogPatternSettings.x<0.5 && fogLens.y<1.5){
   let midPoint=origin+direction*((begin+end)*0.5)-fogLightPosition.xyz
   let radius=fogSpot.w+max(0.0,dot(midPoint,fogLightDirection.xyz))*fogSpot.z
   // At least 16 samples across a normal chord; use the user's higher budget
   // for long/grazing chords. Patterned beams retain the full sample budget.
   sampleCount=min(fogOptions.x,max(16.0,ceil((end-begin)/max(0.02,radius*max(0.01,fogPatternOptics.y)*0.125))))
  }
  if(fogIntegration.y>0.5){sampleCount=1.0}
  let ds=max(end-begin,0.0)/sampleCount
  let transmission=exp(-fogMedium.x*max(begin-mediumBegin,0.0))
  if((fogOptions.z>0.5 || fogNoise.x>0.001) && begin>mediumBegin){
   transmission=1.0
   for(k in range(4)){
    let pre=origin+direction*(mediumBegin+(float(k)+0.5)*(begin-mediumBegin)*0.25)
    let weight=1.0
    if(fogOptions.z>0.5){
     let local=abs(pre-fogCenter.xyz)/fogExtent.xyz
     weight=1.0-smoothstep(0.75,1.0,max(max(local.x,local.y),local.z))
    }
    if(fogNoise.x>0.001){
     let p=pre*fogNoise.y+vec3(fogNoise.z*0.17,fogNoise.z*0.11,fogNoise.z*0.14)
     weight=weight*clamp(1.0+fogNoise.x*(0.5*sin(p.x+p.z*0.73)+0.35*sin(p.y*1.7-p.z*1.1)+0.15*sin(p.x*1.3+p.y*0.6)),0.15,1.85)
    }
    transmission=transmission*exp(-fogMedium.x*(begin-mediumBegin)*0.25*weight)
   }
  }
  let radiance=vec3(0.0)
  if(fogOptions.w<0.5 || fogOptions.w>1.5 || fogOptions.z>0.5) {
  for(i in range(int(sampleCount),64)) {
   let fogSample=origin+direction*(begin+(float(i)+0.5)*ds)
   let localWeight=1.0
   if(fogOptions.z>0.5){
    let local=abs(fogSample-fogCenter.xyz)/fogExtent.xyz
    localWeight=1.0-smoothstep(0.75,1.0,max(max(local.x,local.y),local.z))
   }
   if(fogNoise.x>0.001){
    let p=fogSample*fogNoise.y+vec3(fogNoise.z*0.17,fogNoise.z*0.11,fogNoise.z*0.14)
    localWeight=localWeight*clamp(1.0+fogNoise.x*(0.5*sin(p.x+p.z*0.73)+0.35*sin(p.y*1.7-p.z*1.1)+0.15*sin(p.x*1.3+p.y*0.6)),0.15,1.85)
   }
   let stepT=exp(-fogMedium.x*ds*localWeight)
   let toLight=fogLightPosition.xyz-fogSample
   let lightDistance=length(toLight)
   let L=fogLightDirection.xyz*-1.0
   let attenuation=1.0
   if(fogLightPosition.w>0.5) {
    L=toLight/max(lightDistance,0.00001)
    let cutoff=clamp(1.0-lightDistance/max(fogLightDirection.w,0.00001),0.0,1.0)
    attenuation=cutoff*cutoff/max(lightDistance*lightDistance,max(0.01,fogSpot.w*fogSpot.w))
    if(fogLightPosition.w>1.5) {
     let delta=fogSample-fogLightPosition.xyz
  let optical=0.0
  for(lobe in range(int(fogLens.y),8)){
   let beamAxis=performanceLobeDirection(fogLightDirection.xyz,fogPatternRight.xyz,fogPatternUp.xyz,fogLens,float(lobe))
   let reference=vec3(0.0,1.0,0.0)
   if(abs(beamAxis.y)>0.99){reference=vec3(0.0,0.0,1.0)}
   let beamRight=normalize(cross(reference,beamAxis));let beamUp=normalize(cross(beamAxis,beamRight))
   let axial=dot(delta,beamAxis)
   let projected=vec2(dot(delta,beamRight),dot(delta,beamUp))/max(fogSpot.w+axial*fogSpot.z,0.00001)
   let defocus=performanceFocusBlur(axial,fogSpot,fogPatternOptics.y,fogLens)
   let localOptics=fogPatternOptics
   let textureMask=1.0
   if(fogPatternSettings.x>4.5){
    let goboUv=performanceGoboUv(projected,fogPatternSettings)
    let inside=step(0.0,goboUv.x)*step(goboUv.x,1.0)*step(0.0,goboUv.y)*step(goboUv.y,1.0)
    let texel=sampleLod(patternGobo,goboUv,0.0)
    if(fogPatternOptics.x>0.01 || defocus>0.0001){
     let blur=fogPatternOptics.x*0.025+defocus*fogPatternSettings.z*0.5
     texel=(texel+sampleLod(patternGobo,goboUv+vec2(blur,0.0),0.0)
           +sampleLod(patternGobo,goboUv-vec2(blur,0.0),0.0)
           +sampleLod(patternGobo,goboUv+vec2(0.0,blur),0.0)
           +sampleLod(patternGobo,goboUv-vec2(0.0,blur),0.0))*0.2
    }
    textureMask=dot(texel.rgb,vec3(0.2126,0.7152,0.0722))*texel.a*inside
   }
   let pattern=performancePattern(projected,fogPatternSettings,localOptics,textureMask)
   if(defocus>0.0001 && fogPatternSettings.x>0.5 && fogPatternSettings.x<4.5){
    let pupil=defocus
    pattern=(pattern+performancePattern(projected+vec2(pupil,0.0),fogPatternSettings,localOptics,1.0)
                    +performancePattern(projected-vec2(pupil,0.0),fogPatternSettings,localOptics,1.0)
                    +performancePattern(projected+vec2(0.0,pupil),fogPatternSettings,localOptics,1.0)
                    +performancePattern(projected-vec2(0.0,pupil),fogPatternSettings,localOptics,1.0))*0.2
   }
   let beam=performanceBeamProfile(delta,beamAxis,fogSpot,fogPatternRight.w)
   let distribution=1.0
   if(fogIesSettings.x>0.5){distribution=sampleLod(iesAtlas,performanceIesUv(delta,beamAxis,beamRight,beamUp),0.0).r}
   optical=optical+beam*pattern*distribution/fogLens.y
  }
     attenuation=attenuation*optical
    }
   }
   let visibility=1.0
   if(fogShadowParams.w>0.5) {
    let viewDepth=(fogView*vec4(fogSample,1.0)).z
    let clip=fogShadow0*vec4(fogSample,1.0)
    let rect=fogShadowRect0
    if(fogShadowParams.w>1.5 && viewDepth>fogShadowParams.x) {clip=fogShadow1*vec4(fogSample,1.0);rect=fogShadowRect1}
    if(fogShadowParams.w>2.5 && viewDepth>fogShadowParams.y) {clip=fogShadow2*vec4(fogSample,1.0);rect=fogShadowRect2}
    let q=clip.xyz/max(clip.w,0.00001)
    let localUv=vec2(q.x*0.5+0.5,0.5-q.y*0.5)
    let ref=q.z*0.5+0.5
    if(clip.w>0.0 && localUv.x>=0.0 && localUv.x<=1.0 && localUv.y>=0.0 && localUv.y<=1.0 && ref>=0.0 && ref<=1.0) {
     let shadowUv=mix(rect.xy,rect.zw,localUv)
     if(fogShadowArea.x>0.0){
     let tile=rect.zw-rect.xy
     let edge=vec2(fogShadowAreaOptions.y*0.5)
     let receiver=max(clip.w,fogShadowArea.y)
     let searchUv=fogShadowArea.x*max(receiver-fogShadowArea.y,0.0)/max(2.0*fogShadowArea.w*fogShadowArea.y*receiver,0.00001)
     let search=vec2(min(fogShadowAreaOptions.w*fogShadowAreaOptions.y,tile.x*searchUv),min(fogShadowAreaOptions.w*fogShadowAreaOptions.y,tile.y*searchUv))
     let blockerSum=0.0;let blockers=0.0
     let centerDepth=sampleLod(shadowAtlas,shadowUv,0.0).r
     let centerDistance=performanceShadowDistance(centerDepth,fogShadowArea)
     if(centerDepth<0.999999 && centerDistance+fogShadowAreaOptions.z<receiver){blockerSum=centerDistance;blockers=1.0}
     for(blockerTap in range(int(fogShadowAreaOptions.x),16)){
      let tap=clamp(shadowUv+performanceShadowOffset(blockerTap,fogShadowAreaOptions.x)*search,rect.xy+edge,rect.zw-edge)
      let depth=sampleLod(shadowAtlas,tap,0.0).r
      let distance=performanceShadowDistance(depth,fogShadowArea)
      if(depth<0.999999 && distance+fogShadowAreaOptions.z<receiver){blockerSum=blockerSum+distance;blockers=blockers+1.0}
     }
     visibility=1.0
     if(blockers>0.5){
      let blocker=blockerSum/blockers
      let worldRadius=performancePenumbraRadius(fogShadowArea.x,receiver,blocker)
      let radiusUv=worldRadius/max(2.0*fogShadowArea.w*receiver,0.00001)
      let radius=vec2(max(fogPatternOptics.z*fogShadowAreaOptions.y,min(fogShadowAreaOptions.w*fogShadowAreaOptions.y,tile.x*radiusUv)),max(fogPatternOptics.z*fogShadowAreaOptions.y,min(fogShadowAreaOptions.w*fogShadowAreaOptions.y,tile.y*radiusUv)))
      let soft=0.0
      for(filterTap in range(int(fogShadowAreaOptions.x),16)){
       let tap=clamp(shadowUv+performanceShadowOffset(filterTap,fogShadowAreaOptions.x)*radius,rect.xy+edge,rect.zw-edge)
       let depth=sampleLod(shadowAtlas,tap,0.0).r
       let distance=performanceShadowDistance(depth,fogShadowArea)
       soft=soft+max(step(receiver-fogShadowAreaOptions.z,distance),step(0.999999,depth))
      }
      visibility=soft/fogShadowAreaOptions.x
     }
    } else if(fogPatternOptics.z<0.001){
      let depth=sampleLod(shadowAtlas,shadowUv,0.0).r
      visibility=max(1.0-step(depth+fogShadowParams.z,ref),step(0.999,depth))
     } else {
      let radius=fogPatternOptics.z*fogPatternOptics.w
      let edge=vec2(fogPatternOptics.w*0.5)
      let soft=0.0
      for(j in range(4)) {
       let ox=-1.0;let oy=-1.0
       if(j==1 || j==3){ox=1.0}
       if(j>=2){oy=1.0}
       let tap=clamp(shadowUv+vec2(ox,oy)*radius,rect.xy+edge,rect.zw-edge)
       let depth=sampleLod(shadowAtlas,tap,0.0).r
       soft=soft+max(1.0-step(depth+fogShadowParams.z,ref),step(0.999,depth))
      }
      visibility=soft*0.25
     }
    }
   }
   let g=fogMedium.z
   // Incoming propagation is -L and outgoing propagation is -direction.
   let mu=dot(L,direction)
   let phaseBase=max(1.0+g*g-2.0*g*mu,0.001)
   let phase=(1.0-g*g)/(12.5663706*phaseBase*sqrt(phaseBase))
   if(fogIntegration.y>0.5){radiance=radiance+fogMedium.y*fogTint.rgb*fogLightColor.rgb*attenuation*visibility*phase}
   else {radiance=radiance+transmission*(1.0-stepT)*fogMedium.y*fogTint.rgb*fogLightColor.rgb*attenuation*visibility*phase}
   transmission=transmission*stepT
  }
  } else {
   transmission=exp(-fogMedium.x*max(end-begin,0.0))
   if(fogNoise.x>0.001){
    transmission=1.0
    for(k in range(4)){
     let pre=origin+direction*(begin+(float(k)+0.5)*(end-begin)*0.25)
     let p=pre*fogNoise.y+vec3(fogNoise.z*0.17,fogNoise.z*0.11,fogNoise.z*0.14)
     let weight=clamp(1.0+fogNoise.x*(0.5*sin(p.x+p.z*0.73)+0.35*sin(p.y*1.7-p.z*1.1)+0.15*sin(p.x*1.3+p.y*0.6)),0.15,1.85)
     transmission=transmission*exp(-fogMedium.x*max(end-begin,0.0)*0.25*weight)
    }
   }
  }
  let result=vec4(raw.rgb*transmission+radiance,raw.a)
  if(fogOptions.w>1.5){result=vec4(radiance,0.0)}
  if(fogOptions.y>1.5) {result=vec4(radiance,raw.a)}
  else if(fogOptions.y>0.5) {result=vec4(vec3(transmission),raw.a)}
  if(fogIntegration.x>0.5){
   result=vec4(radiance,transmission)
   if(fogOptions.w>1.5){result=vec4(radiance,0.0)}
  }
  return result
 }
}
)PHOSKIA";
inline const char* const kVolumetricPhoskiaSource=kVolumetricPhoskiaOwned.c_str();
}
