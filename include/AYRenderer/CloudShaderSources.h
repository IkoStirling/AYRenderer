#pragma once
namespace ayt::render {
inline constexpr char kCloudCacheKey[]="cloud_slab_atlas_v1";
inline constexpr char kCloudPhoskiaSource[]=R"PHOSKIA(
// Convert periodic 3D coordinates into two atlas slices, with wrapped texel borders.
fn cloudFraction(v:float):float {return v-floor(v)}
fn cloudAtlasUv(p:vec3,slice:float):vec2 {
 let z=cloudFraction(slice/32.0)*32.0
 return (vec2(cloudFraction(z/8.0)*8.0,floor(z/8.0))*34.0+vec2(cloudFraction(p.x),cloudFraction(p.y))*32.0+vec2(1.0))/vec2(272.0,136.0)
}
material Clouds {
 texture2d sceneColor
 texture2d worldPosition
 texture2d geometryCoverage
 texture2d shapeNoise
 texture2d detailNoise
 texture2d weatherMap
 uniform vec4 cloudFar0
 uniform vec4 cloudFarX
 uniform vec4 cloudFarY
 uniform vec4 cloudNear0
 uniform vec4 cloudNearX
 uniform vec4 cloudNearY
 uniform vec4 cloudCamera
 uniform vec4 cloudLayer
 uniform vec4 cloudShape
 uniform vec4 cloudOptics
 uniform vec4 cloudTint
 uniform vec4 cloudPhase
 uniform vec4 cloudDetailPhase
 uniform vec4 cloudLightDirection
 uniform vec4 cloudLightColor
 uniform vec4 cloudAmbient
 uniform vec4 cloudQuality
 vertex {in pos:position;out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5);return vec4(pos.xy,0.0,1.0)}
 fragment {
  in vUv:texcoord
  let uv=vec2(vUv.x,1.0-vUv.y)
  let raw=sample(sceneColor,uv)
  let wp=sample(worldPosition,uv)
  let covered=sample(geometryCoverage,uv).a
  let farH=cloudFar0+(cloudFarX-cloudFar0)*uv.x+(cloudFarY-cloudFar0)*uv.y
  let nearH=cloudNear0+(cloudNearX-cloudNear0)*uv.x+(cloudNearY-cloudNear0)*uv.y
  let origin=mix(cloudCamera.xyz,nearH.xyz/nearH.w,cloudCamera.w)
  let direction=normalize(farH.xyz/farH.w-origin)
  let limit=cloudQuality.z
  if(covered>0.5){limit=min(limit,length(wp.xyz-origin))}
  let begin=0.0
  let end=limit
  if(abs(direction.y)>0.00001){
   let a=(cloudLayer.x-origin.y)/direction.y
   let b=(cloudLayer.x+cloudLayer.y-origin.y)/direction.y
   begin=max(0.0,min(a,b));end=min(end,max(a,b))
  }else if(origin.y<cloudLayer.x || origin.y>cloudLayer.x+cloudLayer.y){end=0.0}
  let transmission=1.0
  let radiance=vec3(0.0)
  if(end>begin){
   let ds=(end-begin)/cloudQuality.x
   let rayOffset=0.2+0.6*cloudFraction(sin(uv.x*1234.56+uv.y*7890.12)*43758.5453)
   let lightDs=cloudLayer.y/max(cloudQuality.y,1.0)
   let L=cloudLightDirection.xyz*-1.0
   let g=cloudOptics.z
   let phase=(1.0-g*g)/pow(max(1.0+g*g-2.0*g*dot(L,direction),0.05),1.5)
   for(i in range(int(cloudQuality.x),64)){
    if(transmission>0.005){
     let rayDistance=begin+(float(i)+rayOffset)*ds
     let cloudSample=origin+direction*rayDistance
     let medium=0.0
     let lightDepth=0.0
     for(j in range(int(cloudQuality.y)+1,9)){
      let p=cloudSample+L*(float(j)*lightDs)
      let h=(p.y-cloudLayer.x)/cloudLayer.y
      let density=0.0
      if(h>0.0 && h<1.0){
       let q=p/cloudShape.x+cloudPhase.xyz
       q=q+vec3(sin(q.z*0.31+q.y*0.71),sin(q.x*0.29+q.z*0.47),sin(q.y*0.37+q.x*0.53))*0.2
       let qd=p/cloudShape.y+cloudDetailPhase.xyz
       let z=cloudFraction(q.z)*32.0-0.5
       let zd=cloudFraction(qd.z)*32.0-0.5
       let n=mix(sampleLod(shapeNoise,cloudAtlasUv(q,floor(z)),0.0).r,sampleLod(shapeNoise,cloudAtlasUv(q,floor(z)+1.0),0.0).r,cloudFraction(z))
       let d=mix(sampleLod(detailNoise,cloudAtlasUv(qd,floor(zd)),0.0).r,sampleLod(detailNoise,cloudAtlasUv(qd,floor(zd)+1.0),0.0).r,cloudFraction(zd))
       let weather=sampleLod(weatherMap,vec2(cloudFraction(q.x*0.25),cloudFraction(q.z*0.25)),0.0).r
       let shape=clamp((n-(1.0-cloudLayer.z*weather)*0.7)/0.3-d*cloudShape.z*0.35,0.0,1.0)*smoothstep(0.0,0.1,cloudLayer.z*weather)
       density=shape*smoothstep(0.0,0.12,h)*(1.0-smoothstep(0.55,1.0,h))*cloudLayer.w
      }
      if(j==0){medium=density*(1.0-smoothstep(cloudQuality.z*0.65,cloudQuality.z,rayDistance))}else{lightDepth=lightDepth+density*lightDs}
     }
     let stepT=exp(-medium*cloudOptics.x*ds)
     let lightT=mix(1.0,exp(-lightDepth*cloudOptics.x),cloudOptics.w)
     let height=clamp((cloudSample.y-cloudLayer.x)/cloudLayer.y,0.0,1.0)
     let lighting=cloudAmbient.rgb*(0.35+height*0.65)+cloudLightColor.rgb*lightT*phase*0.8
     radiance=radiance+transmission*(1.0-stepT)*cloudOptics.y*cloudTint.rgb*lighting
     transmission=transmission*stepT
    }
   }
  }
  let result=vec4(raw.rgb*transmission+radiance,raw.a)
  if(cloudQuality.w>1.5){result=vec4(radiance,raw.a)}else if(cloudQuality.w>0.5){result=vec4(vec3(transmission),raw.a)}
  if(cloudQuality.w>2.5){result=vec4(direction*0.5+vec3(0.5),1.0)}
  return result
 }
}
)PHOSKIA";
}
