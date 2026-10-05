#pragma once
namespace ayt::render {
inline constexpr char kVolumetricCacheKey[]="volumetric_v2_forward_phase";
inline constexpr char kVolumetricPhoskiaSource[]=R"PHOSKIA(
material Volumetric {
 texture2d sceneColor
 texture2d worldPosition
 texture2d geometryCoverage
 texture2d shadowAtlas
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
 uniform vec4 fogLightPosition
 uniform vec4 fogLightDirection
 uniform vec4 fogLightColor
 uniform vec4 fogSpot
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
  let ds=max(end-begin,0.0)/fogOptions.x
  let stepT=exp(-fogMedium.x*ds)
  let transmission=1.0
  let radiance=vec3(0.0)
  for(i in range(int(fogOptions.x),64)) {
   let fogSample=origin+direction*(begin+(float(i)+0.5)*ds)
   let toLight=fogLightPosition.xyz-fogSample
   let lightDistance=length(toLight)
   let L=fogLightDirection.xyz*-1.0
   let attenuation=1.0
   if(fogLightPosition.w>0.5) {
    L=toLight/max(lightDistance,0.00001)
    let cutoff=clamp(1.0-lightDistance/max(fogLightDirection.w,0.00001),0.0,1.0)
    attenuation=cutoff*cutoff/max(lightDistance*lightDistance,0.01)
    if(fogLightPosition.w>1.5) {attenuation=attenuation*smoothstep(fogSpot.y,max(fogSpot.x,fogSpot.y+0.0001),dot(L*-1.0,fogLightDirection.xyz))}
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
     let depth=sampleLod(shadowAtlas,shadowUv,0.0).r
     visibility=max(1.0-step(depth+fogShadowParams.z,ref),step(0.999,depth))
    }
   }
   let g=fogMedium.z
   // Incoming propagation is -L and outgoing propagation is -direction.
   let mu=dot(L,direction)
   let phase=(1.0-g*g)/(12.5663706*pow(max(1.0+g*g-2.0*g*mu,0.001),1.5))
   radiance=radiance+transmission*(1.0-stepT)*fogMedium.y*fogTint.rgb*fogLightColor.rgb*attenuation*visibility*phase
   transmission=transmission*stepT
  }
  let result=vec4(raw.rgb*transmission+radiance,raw.a)
  if(fogOptions.y>1.5) {result=vec4(radiance,raw.a)}
  else if(fogOptions.y>0.5) {result=vec4(vec3(transmission),raw.a)}
  return result
 }
}
)PHOSKIA";
}
