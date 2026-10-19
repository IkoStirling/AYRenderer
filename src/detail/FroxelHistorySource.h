#pragma once
namespace ayt::render::detail {
inline constexpr char kFroxelHistoryKey[]="froxel_history_v1";
inline constexpr char kFroxelHistorySource[]=R"(
material FroxelHistory {
 texture2d currentField
 texture2d previousField
 uniform mat4 historyInverseViewProjection
 uniform mat4 historyPreviousViewProjection
 uniform vec4 historyCamera
 uniform vec4 historyPreviousCamera
 uniform vec4 historyPreviousDirection
 uniform vec4 historyPacking
 uniform vec4 historyOptions
 uniform vec4 historyGrid
 vertex {in pos:position;out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5);return vec4(pos.xy,0.0,1.0)}
 fragment {
  in vUv:texcoord
  let at=vec2(vUv.x,1.0-vUv.y)
  let current=sampleLod(currentField,at,0.0)
  let result=current
  if(historyOptions.y>0.5){
   let tile=vec2(floor(at.x*historyPacking.x),floor(at.y*historyPacking.y))
   let slice=tile.y*historyPacking.x+tile.x
   let uv=at*historyPacking.xy-tile
   let farH=historyInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,1.0,1.0)
   let nearH=historyInverseViewProjection*vec4(uv.x*2.0-1.0,1.0-uv.y*2.0,historyOptions.z,1.0)
   let origin=mix(historyCamera.xyz,nearH.xyz/nearH.w,historyCamera.w)
   let delta=farH.xyz/farH.w-origin
   let a=exp(log(1.0+historyPacking.w)*slice/historyPacking.z)-1.0
   let b=min(length(delta),exp(log(1.0+historyPacking.w)*(slice+1.0)/historyPacking.z)-1.0)
   let world=origin+delta/max(length(delta),0.00001)*((a+b)*0.5)
   let clip=historyPreviousViewProjection*vec4(world,1.0)
   let oldUv=vec2(clip.x/max(clip.w,0.00001)*0.5+0.5,0.5-clip.y/max(clip.w,0.00001)*0.5)
   let distance=length(world-historyPreviousCamera.xyz)
   if(historyCamera.w>0.5){distance=dot(world-historyPreviousCamera.xyz,historyPreviousDirection.xyz)}
   if(clip.w>0.0&&min(oldUv.x,oldUv.y)>0.0&&max(oldUv.x,oldUv.y)<1.0&&distance>0.0&&distance<historyPacking.w){
    let oldSlice=clamp(floor(log(1.0+distance)/log(1.0+historyPacking.w)*historyPacking.z),0.0,historyPacking.z-1.0)
    let oldTileY=floor(oldSlice/historyPacking.x)
    let oldTileX=oldSlice-oldTileY*historyPacking.x
    let safeUv=vec2(clamp(oldUv.x,historyGrid.x*0.5,1.0-historyGrid.x*0.5),clamp(oldUv.y,historyGrid.y*0.5,1.0-historyGrid.y*0.5))
    let old=sampleLod(previousField,vec2((oldTileX+safeUv.x)/historyPacking.x,(oldTileY+safeUv.y)/historyPacking.y),0.0)
    let texel=historyGrid.xy/historyPacking.xy
    let lower=(tile+historyGrid.xy*0.5)/historyPacking.xy
    let upper=(tile+vec2(1.0)-historyGrid.xy*0.5)/historyPacking.xy
    let lo=vec3(current.rgb);let hi=vec3(current.rgb)
    for(i in range(4)){
     let offset=vec2(0.0)
     if(i==0){offset=vec2(texel.x,0.0)}
     if(i==1){offset=vec2(-texel.x,0.0)}
     if(i==2){offset=vec2(0.0,texel.y)}
     if(i==3){offset=vec2(0.0,-texel.y)}
     let candidate=at+offset
     let neighborUv=vec2(clamp(candidate.x,lower.x,upper.x),clamp(candidate.y,lower.y,upper.y))
     let neighbor=sampleLod(currentField,neighborUv,0.0).rgb
     lo=min(lo,neighbor);hi=max(hi,neighbor)
    }
    let luminance=dot(current.rgb,vec3(0.2126,0.7152,0.0722))
    let previousLuminance=dot(old.rgb,vec3(0.2126,0.7152,0.0722))
    let weight=historyOptions.x
    if(luminance<0.000001||abs(previousLuminance-luminance)>luminance*0.5||abs(old.a-current.a)>max(current.a*0.1,0.00001)){weight=0.0}
    let clamped=vec3(clamp(old.r,lo.r,hi.r)*step(0.000001,current.r),clamp(old.g,lo.g,hi.g)*step(0.000001,current.g),clamp(old.b,lo.b,hi.b)*step(0.000001,current.b))
    result=vec4(mix(current.rgb,clamped,weight),current.a)
   }
  }
  return result
 }
}
)";
}
