#pragma once
namespace ayt::render {
inline constexpr char kCloudCacheKey[]="cloud_weather_cumulus_v2";
inline constexpr char kCloudPhoskiaSource[]=R"PHOSKIA(
// Convert periodic 3D coordinates into two atlas slices, with wrapped texel borders.
fn cloudFraction(v:float):float {return v-floor(v)}
fn cloudAtlasUv(p:vec3,slice:float,layout:vec4):vec2 {
 let z=cloudFraction(slice/layout.x)*layout.x
 return (vec2(cloudFraction(z/layout.z)*layout.z,floor(z/layout.z))*layout.y+vec2(cloudFraction(p.x),cloudFraction(p.y))*layout.x+vec2(1.0))/(layout.zw*layout.y)
}
fn cloudCellHash(cell:vec2,seed:float):vec3 {
 return vec3(cloudFraction(sin(dot(cell,vec2(127.1,311.7))+seed)*43758.5453),cloudFraction(sin(dot(cell,vec2(269.5,183.3))+seed)*22578.1459),cloudFraction(sin(dot(cell,vec2(419.2,371.9))+seed)*19642.3491))
}
// A world-space cluster consists of overlapping unequal lobes. The independent volume
// erodes their boundary; a cloud cannot collapse into a globally uniform height sheet.
fn cloudCellBody(p:vec3,cell:vec2,seed:float):float {
 let r=cloudCellHash(cell,seed)
 let center=vec3(cell.x+0.5+(r.x-0.5)*0.6,0.23+r.y*0.43,cell.y+0.5+(r.z-0.5)*0.6)
 let radius=vec3(0.17+r.z*0.25,min(0.16+r.x*0.25,0.82-center.y),0.17+r.y*0.25)
 let a=1.0-length((p-center)/radius)
 let b=1.0-length((p-center-vec3(0.14,0.16,-0.07))/(radius*vec3(0.72,0.85,0.72)))
 let c=1.0-length((p-center-vec3(-0.13,0.09,0.1))/(radius*vec3(0.75,0.7,0.8)))
 return max(max(a,b),c)-(1.0-smoothstep(0.12,0.35,r.x))*2.0
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
 uniform vec4 cloudShapeLayout
 uniform vec4 cloudDetailLayout
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
  let nearRelative=nearH.xyz/nearH.w*cloudCamera.w
  let origin=cloudCamera.xyz+nearRelative
  let direction=normalize(farH.xyz/farH.w-nearRelative)
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
   let rayOffset=cloudFraction(sin(uv.x*1234.56+uv.y*7890.12)*43758.5453)
   let L=cloudLightDirection.xyz*-1.0
   let g=cloudOptics.z
   let mu=dot(L,direction)
   let phase=0.8*(1.0-g*g)/pow(max(1.0+g*g-2.0*g*mu,0.05),1.5)+0.2*0.9375/pow(1.0625+0.5*mu,1.5)
   // Phoskia bounds each loop to 64; two chunks of 48 keep total nesting at 864.
   for(chunk in range(2)){
   for(i in range(int(min(cloudQuality.x,48.0)),48)){
    let sampleIndex=float(chunk*48+i)
    if(transmission>0.005 && sampleIndex<cloudQuality.x){
     let rayDistance=begin+(sampleIndex+rayOffset)*ds
     let cloudSample=origin+direction*rayDistance
     let medium=0.0
     let lightDepth=0.0
     for(j in range(int(cloudQuality.y)+1,9)){
      let lightU=float(j)/max(cloudQuality.y,1.0)
      let lightV=max(float(j)-1.0,0.0)/max(cloudQuality.y,1.0)
      let lightDs=(lightU*lightU-lightV*lightV)*cloudLayer.y*1.5
      let p=cloudSample+L*((lightU*lightU+lightV*lightV)*cloudLayer.y*0.75)
      let h=(p.y-cloudLayer.x)/cloudLayer.y
      let density=0.0
      if(h>0.0 && h<1.0 && (j==0 || medium>0.0001)){
       let q=p/cloudShape.x+cloudPhase.xyz
       // Periodic warp: advancing any normalized phase by one remains continuous.
       q=q+vec3(sin(q.z*6.283185),sin(q.x*6.283185),sin(q.y*6.283185))*0.035
       let qd=p/cloudShape.y+cloudDetailPhase.xyz
       let z=cloudFraction(q.z)*cloudShapeLayout.x-0.5
       let zd=cloudFraction(qd.z)*cloudDetailLayout.x-0.5
       let n=mix(sampleLod(shapeNoise,cloudAtlasUv(q,floor(z),cloudShapeLayout),0.0),sampleLod(shapeNoise,cloudAtlasUv(q,floor(z)+1.0,cloudShapeLayout),0.0),cloudFraction(z))
       let d=mix(sampleLod(detailNoise,cloudAtlasUv(qd,floor(zd),cloudDetailLayout),0.0).rgb,sampleLod(detailNoise,cloudAtlasUv(qd,floor(zd)+1.0,cloudDetailLayout),0.0).rgb,cloudFraction(zd))
       // World-space macro weather is independent of the short, wrapping volume phase.
       let macro=vec2(p.x*0.93+p.z*0.37,p.z*0.93-p.x*0.37)/(cloudShape.x*6.0)+vec2(0.37,0.63)
       let weather=sampleLod(weatherMap,vec2(cloudFraction(macro.x),cloudFraction(macro.y)),0.0).r
       let local=clamp((weather-(1.0-cloudLayer.z))/0.35,0.0,1.0)*smoothstep(0.0,0.03,cloudLayer.z)
       let top=0.28+0.72*local
       let profile=smoothstep(0.0,0.07,h)*(1.0-smoothstep(top*0.65,top,h))
       let body=clamp(n.r+(dot(n.gba,vec3(0.625,0.25,0.125))-0.5)*0.25,0.0,1.0)
       let threshold=1.0-local*0.52
       let layer=clamp((body*profile-threshold)/0.18,0.0,1.0)
       let formation=vec3(p.x/cloudShape.x,h,p.z/cloudShape.x)
       let cell=vec2(floor(formation.x-0.5),floor(formation.z-0.5))
       let lobes=max(max(cloudCellBody(formation,cell,cloudTint.w),cloudCellBody(formation,cell+vec2(1.0,0.0),cloudTint.w)),max(cloudCellBody(formation,cell+vec2(0.0,1.0),cloudTint.w),cloudCellBody(formation,cell+vec2(1.0),cloudTint.w)))
       let puff=clamp((lobes+(body-0.5)*0.9-0.08)*4.0,0.0,1.0)*smoothstep(0.05,0.3,body)*smoothstep(0.0,0.35,local)
       let shape=mix(puff,layer,smoothstep(0.75,0.95,cloudLayer.z))
       let erosion=dot(d,vec3(0.625,0.25,0.125))*cloudShape.z*0.25*(1.0-shape)
       density=max(shape-erosion,0.0)*cloudLayer.w*3.0
      }
      if(j==0){medium=density*(1.0-smoothstep(cloudQuality.z*0.65,cloudQuality.z,rayDistance))}else{lightDepth=lightDepth+density*lightDs}
     }
     let stepT=exp(-medium*cloudOptics.x*ds)
     let lightT=mix(1.0,exp(-lightDepth*cloudOptics.x),cloudOptics.w)
     let height=clamp((cloudSample.y-cloudLayer.x)/cloudLayer.y,0.0,1.0)
     // Two attenuated scattering orders approximate energy reaching thick cloud interiors.
     let scattered=exp(-lightDepth*cloudOptics.x*0.25)
     let multiple=mix(1.0,scattered,cloudOptics.w)
     let lighting=cloudAmbient.rgb*(0.7+height*0.6)+cloudLightColor.rgb*(lightT*phase*0.65+multiple*0.15)
     let aerial=1.0-exp(-rayDistance/65000.0)
     let lit=cloudOptics.y*mix(cloudTint.rgb*lighting,raw.rgb,aerial)
     radiance=radiance+transmission*(1.0-stepT)*lit
     transmission=transmission*stepT
    }
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
