#pragma once
#include "AYRenderer/PerformanceShaderSources.h"
namespace ayt::render {
inline constexpr char kVolumetricCacheKey[]="volumetric_v11_ies";
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
  let raw=sample(sceneColor,uv)
  let wp=sample(worldPosition,uv)
  let coverage=sample(geometryCoverage,uv).a
  let farH=fogInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,1.0,1.0)
  let nearH=fogInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,fogCenter.w,1.0)
  let origin=mix(fogCamera.xyz,nearH.xyz/nearH.w,fogCamera.w)
  let endPoint=mix(farH.xyz/farH.w,wp.xyz,step(0.5,coverage))
  let delta=endPoint-origin
  let distance=min(length(delta),fogMedium.w)
  let direction=delta/max(length(delta),0.00001)
  let begin=0.0
  let end=distance
  if (fogOptions.z>0.5) {
   let safeDirection=mix(vec3(0.00001),direction,step(vec3(0.00001),abs(direction)))
   let a=(fogCenter.xyz-fogExtent.xyz-origin)/safeDirection
   let b=(fogCenter.xyz+fogExtent.xyz-origin)/safeDirection
   begin=max(0.0,max(max(min(a.x,b.x),min(a.y,b.y)),min(a.z,b.z)))
   end=min(distance,min(min(max(a.x,b.x),max(a.y,b.y)),max(a.z,b.z)))
  }
  let mediumBegin=begin
  if(fogOptions.w>1.5){
   let safeDirection=mix(vec3(0.00001),direction,step(vec3(0.00001),abs(direction)))
   let a=(fogLightBoundsCenter.xyz-fogLightBoundsExtent.xyz-origin)/safeDirection
   let b=(fogLightBoundsCenter.xyz+fogLightBoundsExtent.xyz-origin)/safeDirection
   begin=max(begin,max(max(min(a.x,b.x),min(a.y,b.y)),min(a.z,b.z)))
   end=min(end,min(min(max(a.x,b.x),max(a.y,b.y)),max(a.z,b.z)))
   if(fogLightPosition.w>1.5){
    let segment=performanceBeamInterval(origin,direction,fogLightPosition.xyz,fogLightDirection.xyz,fogSpot,fogLightDirection.w,vec2(begin,end))
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
  if(fogOptions.w>1.5 && fogLightPosition.w>1.5 && fogPatternSettings.x<0.5){
   let midPoint=origin+direction*((begin+end)*0.5)-fogLightPosition.xyz
   let radius=fogSpot.w+max(0.0,dot(midPoint,fogLightDirection.xyz))*fogSpot.z
   // At least 16 samples across a normal chord; use the user's higher budget
   // for long/grazing chords. Patterned beams retain the full sample budget.
   sampleCount=min(fogOptions.x,max(16.0,ceil((end-begin)/max(0.02,radius*max(0.01,fogPatternOptics.y)*0.125))))
  }
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
     attenuation=attenuation*performanceBeamProfile(delta,fogLightDirection.xyz,fogSpot,fogPatternRight.w)
     let axial=dot(delta,fogLightDirection.xyz)
     let projected=vec2(dot(delta,fogPatternRight.xyz),dot(delta,fogPatternUp.xyz))/max(fogSpot.w+axial*fogSpot.z,0.00001)
     let textureMask=1.0
     if(fogPatternSettings.x>4.5){
      let goboUv=performanceGoboUv(projected,fogPatternSettings)
      let inside=step(0.0,goboUv.x)*step(goboUv.x,1.0)*step(0.0,goboUv.y)*step(goboUv.y,1.0)
      let texel=sampleLod(patternGobo,goboUv,0.0)
      if(fogPatternOptics.x>0.01){
       let blur=fogPatternOptics.x*0.025
       texel=(texel+sampleLod(patternGobo,goboUv+vec2(blur,0.0),0.0)
             +sampleLod(patternGobo,goboUv-vec2(blur,0.0),0.0)
             +sampleLod(patternGobo,goboUv+vec2(0.0,blur),0.0)
             +sampleLod(patternGobo,goboUv-vec2(0.0,blur),0.0))*0.2
      }
      textureMask=dot(texel.rgb,vec3(0.2126,0.7152,0.0722))*texel.a*inside
     }
     if(fogIesSettings.x>0.5){attenuation=attenuation*sampleLod(iesAtlas,performanceIesUv(delta,fogLightDirection.xyz,fogPatternRight.xyz,fogPatternUp.xyz),0.0).r}
     attenuation=attenuation*performancePattern(projected,fogPatternSettings,fogPatternOptics,textureMask)
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
     if(fogPatternOptics.z<0.001){
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
   radiance=radiance+transmission*(1.0-stepT)*fogMedium.y*fogTint.rgb*fogLightColor.rgb*attenuation*visibility*phase
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
  return result
 }
}
)PHOSKIA";
inline const char* const kVolumetricPhoskiaSource=kVolumetricPhoskiaOwned.c_str();
}
