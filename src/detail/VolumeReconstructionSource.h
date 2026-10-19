#pragma once
#include "AYRenderer/VolumetricDensityShader.h"
#include <string>
namespace ayt::render::detail {
inline constexpr char kVolumeReconstructionKey[]="volume_reconstruction_v3_density_grid";
inline const std::string kVolumeReconstructionOwned=std::string(kVolumetricDensityFunctions)+R"(
material VolumeReconstruction {
 texture2d sceneColor
 texture2d worldPosition
 texture2d geometryCoverage
 texture2d fogIntegral
 texture2d fogDensityAtlas
 uniform vec4 fogDensityGrid
 uniform mat4 fogInverseViewProjection
 uniform mat4 fogView
 uniform vec4 fogCamera
 uniform vec4 fogMedium
 uniform vec4 fogCenter
 uniform vec4 fogExtent
 uniform vec4 fogNoise
 uniform vec4 fogOptions
 uniform vec4 reconstructionGrid
 vertex {in pos:position;out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5);return vec4(pos.xy,0.0,1.0)}
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
  if(fogOptions.z>0.5){
   let safeDirection=mix(vec3(0.00001),direction,step(vec3(0.00001),abs(direction)))
   let a=(fogCenter.xyz-fogExtent.xyz-origin)/safeDirection
   let b=(fogCenter.xyz+fogExtent.xyz-origin)/safeDirection
   begin=max(0.0,max(max(min(a.x,b.x),min(a.y,b.y)),min(a.z,b.z)))
   end=min(distance,min(min(max(a.x,b.x),max(a.y,b.y)),max(a.z,b.z)))
  }
  // Extinction remains full resolution: scene edges never inherit a neighbor's
  // path length, and no downsampled scene RGB is ever displayed.
  let transmission=exp(-fogMedium.x*max(end-begin,0.0))
  if(fogOptions.z>0.5||fogNoise.x>0.001||fogDensityGrid.w>0.5){
   transmission=1.0
   let extinctionSamples=4.0
   if(fogDensityGrid.w>0.5){extinctionSamples=16.0}
   for(k in range(int(extinctionSamples),16)){
    let pre=origin+direction*(begin+(float(k)+0.5)*(end-begin)/extinctionSamples)
    let weight=volumetricMediumWeight(fogDensityAtlas,pre,fogCenter.xyz,fogExtent.xyz,fogOptions,fogNoise,fogDensityGrid)
    transmission=transmission*exp(-fogMedium.x*max(end-begin,0.0)/extinctionSamples*weight)
   }
  }
  let base=vec2(floor(uv.x*reconstructionGrid.x-0.5),floor(uv.y*reconstructionGrid.y-0.5))
  let targetDepth=(fogView*vec4(wp.xyz,1.0)).z
  let tolerance=max(abs(targetDepth)*0.02,0.05)
  let gathered=vec3(0.0)
  let total=0.0
  for(tap in range(4)){
   let offset=vec2(0.0)
   if(tap==1||tap==3){offset.x=1.0}
   if(tap>=2){offset.y=1.0}
   let candidate=(base+offset+vec2(0.5))*reconstructionGrid.zw
   let at=vec2(clamp(candidate.x,reconstructionGrid.z*0.5,1.0-reconstructionGrid.z*0.5),clamp(candidate.y,reconstructionGrid.w*0.5,1.0-reconstructionGrid.w*0.5))
   let lowCoverage=sampleLod(geometryCoverage,at,0.0).a
   let lowWorld=sampleLod(worldPosition,at,0.0).xyz
   let lowDepth=(fogView*vec4(lowWorld,1.0)).z
   let sameClass=1.0-abs(step(0.5,lowCoverage)-step(0.5,coverage))
   let separation=abs(lowDepth-targetDepth)*step(0.5,coverage)
   let spatial=vec2(max(1.0-abs((uv.x-at.x)*reconstructionGrid.x),0.0),max(1.0-abs((uv.y-at.y)*reconstructionGrid.y),0.0))
   let weight=spatial.x*spatial.y*sameClass*exp(-separation/tolerance)
   // Reject disoccluded/thin receivers rather than leak a background beam.
   weight=weight*(1.0-step(tolerance*4.0,separation))
   gathered=gathered+sampleLod(fogIntegral,at,0.0).rgb*weight
   total=total+weight
  }
  let radiance=gathered/max(total,0.000001)
  let result=vec4(raw.rgb*transmission+radiance,raw.a)
  if(fogOptions.y>1.5){result=vec4(radiance,raw.a)}
  else if(fogOptions.y>0.5){result=vec4(vec3(transmission),raw.a)}
  return result
 }
}
)";
inline const char* kVolumeReconstructionSource=kVolumeReconstructionOwned.c_str();
}
