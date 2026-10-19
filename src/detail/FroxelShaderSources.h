#pragma once
#include "AYRenderer/VolumetricDensityShader.h"
#include <string>
namespace ayt::render::detail {
inline constexpr char kFroxelDensityKey[]="froxel_density_v2_authored_grid";
inline const std::string kFroxelDensityOwned=std::string(kVolumetricDensityFunctions)+R"(
material FroxelDensity {
 texture2d fogDensityAtlas
 uniform vec4 fogDensityGrid
 uniform mat4 fogInverseViewProjection
 uniform vec4 fogCamera
 uniform vec4 fogMedium
 uniform vec4 fogCenter
 uniform vec4 fogExtent
 uniform vec4 fogNoise
 uniform vec4 fogOptions
 uniform vec4 froxelPacking
 vertex {in pos:position;out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5);return vec4(pos.xy,0.0,1.0)}
 fragment {
  in vUv:texcoord
  let atlasUv=vec2(vUv.x,1.0-vUv.y)
  let tileX=floor(atlasUv.x*froxelPacking.x)
  let tileY=floor(atlasUv.y*froxelPacking.y)
  let cellIndex=tileY*froxelPacking.x+tileX
  let uv=vec2(atlasUv.x*froxelPacking.x-tileX,atlasUv.y*froxelPacking.y-tileY)
  let farH=fogInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,1.0,1.0)
  let nearH=fogInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,fogCenter.w,1.0)
  let origin=mix(fogCamera.xyz,nearH.xyz/nearH.w,fogCamera.w)
  let delta=farH.xyz/farH.w-origin
  let ray=delta/max(length(delta),0.00001)
  let cellBegin=exp(log(1.0+fogMedium.w)*cellIndex/froxelPacking.z)-1.0
  let cellEnd=min(length(delta),exp(log(1.0+fogMedium.w)*(cellIndex+1.0)/froxelPacking.z)-1.0)
  let result=vec4(0.0)
  if(cellEnd>cellBegin){
   let worldPoint=origin+ray*((cellBegin+cellEnd)*0.5)
   let weight=volumetricMediumWeight(fogDensityAtlas,worldPoint,fogCenter.xyz,fogExtent.xyz,fogOptions,fogNoise,fogDensityGrid)
   result=vec4(0.0,0.0,0.0,fogMedium.x*weight)
  }
  return result
 }
}
)";
inline const char* kFroxelDensitySource=kFroxelDensityOwned.c_str();
inline constexpr char kFroxelIntegrationKey[]="froxel_integration_v1";
inline constexpr char kFroxelIntegrationSource[]=R"(
material FroxelIntegration {
 texture2d froxelField
 texture2d worldPosition
 texture2d geometryCoverage
 uniform mat4 fogInverseViewProjection
 uniform mat4 fogView
 uniform vec4 fogCamera
 uniform vec4 fogMedium
 uniform vec4 fogCenter
 uniform vec4 fogExtent
 uniform vec4 fogOptions
 uniform vec4 froxelPacking
 vertex {in pos:position;out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5);return vec4(pos.xy,0.0,1.0)}
 fragment {
  in vUv:texcoord
  let uv=vec2(vUv.x,1.0-vUv.y)
  let wp=sample(worldPosition,uv)
  let coverage=sample(geometryCoverage,uv).a
  let farH=fogInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,1.0,1.0)
  let nearH=fogInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,fogCenter.w,1.0)
  let origin=mix(fogCamera.xyz,nearH.xyz/nearH.w,fogCamera.w)
  let delta=farH.xyz/farH.w-origin
  let direction=delta/max(length(delta),0.00001)
  let reach=length(delta)
  if(coverage>0.5){
   let sceneZ=(fogView*vec4(wp.xyz,1.0)).z
   let originZ=(fogView*vec4(origin,1.0)).z
   let rayZ=(fogView*vec4(direction,0.0)).z
   reach=max((sceneZ-originZ)/max(rayZ,0.00001),0.0)
  }
  let begin=0.0
  let end=min(reach,fogMedium.w)
  if(fogOptions.z>0.5){
   let safeDirection=mix(vec3(0.00001),direction,step(vec3(0.00001),abs(direction)))
   let a=(fogCenter.xyz-fogExtent.xyz-origin)/safeDirection
   let b=(fogCenter.xyz+fogExtent.xyz-origin)/safeDirection
   begin=max(0.0,max(max(min(a.x,b.x),min(a.y,b.y)),min(a.z,b.z)))
   end=min(end,min(min(max(a.x,b.x),max(a.y,b.y)),max(a.z,b.z)))
  }
  let radiance=vec3(0.0)
  let transmission=1.0
  for(i in range(int(froxelPacking.z),64)){
   let cellBegin=exp(log(1.0+fogMedium.w)*float(i)/froxelPacking.z)-1.0
   let cellEnd=exp(log(1.0+fogMedium.w)*float(i+1)/froxelPacking.z)-1.0
   let segmentBegin=max(begin,cellBegin)
   let segmentEnd=min(end,cellEnd)
   if(segmentEnd>segmentBegin&&transmission>0.000001){
    let tileY=floor(float(i)/froxelPacking.x)
    let tileX=float(i)-tileY*froxelPacking.x
    let at=vec2((tileX+uv.x)/froxelPacking.x,(tileY+uv.y)/froxelPacking.y)
    let value=sampleLod(froxelField,at,0.0)
    let stepT=exp(-max(value.a,0.0)*(segmentEnd-segmentBegin))
    radiance=radiance+transmission*(1.0-stepT)*value.rgb
    transmission=transmission*stepT
   }
  }
  return vec4(radiance,transmission)
 }
}
)";
}
