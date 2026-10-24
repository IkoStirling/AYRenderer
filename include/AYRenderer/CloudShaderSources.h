#pragma once
namespace ayt::render {
inline constexpr char kCloudCacheKey[]="cloud_static_families_v4";
inline constexpr char kCloudPhoskiaSource[]=R"PHOSKIA(
// Convert periodic 3D coordinates into two atlas slices, with wrapped texel borders.
fn cloudFraction(v:float):float {return v-floor(v)}
fn cloudAtlasUv(p:vec3,slice:float,layout:vec4):vec2 {
 let z=cloudFraction(slice/layout.x)*layout.x
 return (vec2(cloudFraction(z/layout.z)*layout.z,floor(z/layout.z))*layout.y+vec2(cloudFraction(p.x),cloudFraction(p.y))*layout.x+vec2(1.0))/(layout.zw*layout.y)
}
fn cloudHash2(p:vec2):float {return cloudFraction(sin(dot(p,vec2(127.1,311.7)))*43758.5453)}
fn cloudNoise2(p:vec2):float {
 let i=floor(p)
 let f=p-i
 let u=f*f*(vec2(3.0)-f*2.0)
 return mix(mix(cloudHash2(i),cloudHash2(i+vec2(1.0,0.0)),u.x),mix(cloudHash2(i+vec2(0.0,1.0)),cloudHash2(i+vec2(1.0,1.0)),u.x),u.y)
}
fn cloudFbm2(p:vec2):float {
 let r=vec2(p.x*0.8-p.y*0.6,p.x*0.6+p.y*0.8)
 return cloudNoise2(p)*0.55+cloudNoise2(r*2.13+vec2(17.1,9.2))*0.28+cloudNoise2(p*4.31+vec2(3.7,21.4))*0.17
}
fn cloudWisps(p:vec2,seed:float):float {
 let q=p+vec2(cloudFraction(seed*0.1733),cloudFraction(seed*0.2377))*31.0
 let warp=cloudFbm2(q*0.55+vec2(5.1,1.7))
 let curl=cloudFbm2(q*0.47+vec2(19.7,3.2))
 let broad=cloudFbm2(q*0.9)
 let strands=cloudFbm2(vec2(q.x*0.75+warp*2.0,q.y*10.0+curl*3.5))
 return smoothstep(0.3,0.68,broad)*pow(clamp(1.0-abs(strands-0.5)*2.0,0.0,1.0),5.0)
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
 uniform vec4 cloudMorphology
 uniform vec4 cloudHorizon
 uniform vec4 cloudFieldBounds
 uniform vec4 cloudFieldControl
 uniform vec4 cloudWeatherPolicy
 uniform vec4 cloudForm
 uniform vec4 cloudHighLayer
 uniform vec4 cloudHighShape
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
  let background=raw.rgb
  if(covered<0.5 && cloudHorizon.w>0.5){
   let horizon=exp(-max(direction.y,0.0)*5.0)
   background=mix(raw.rgb,cloudHorizon.rgb,horizon)
  }
  let limit=cloudQuality.z
  if(covered>0.5){limit=min(limit,length(wp.xyz-origin))}
  let begin=0.0
  let end=limit
  if(abs(direction.y)>0.00001){
   let bottom=cloudLayer.x
   if(cloudFieldControl.x>0.5){bottom=min(bottom,0.0)}
   let a=(bottom-origin.y)/direction.y
   let b=(cloudLayer.x+cloudLayer.y-origin.y)/direction.y
   begin=max(0.0,min(a,b));end=min(end,max(a,b))
  }else if(origin.y<min(cloudLayer.x,0.0) || origin.y>cloudLayer.x+cloudLayer.y){end=0.0}
  let transmission=1.0
  let radiance=vec3(0.0)
  if(end>begin && cloudLayer.w>0.0 && cloudLayer.z>0.0){
   let ds=(end-begin)/cloudQuality.x
   let rayOffset=cloudFraction(cloudFraction(sin(uv.x*1234.56+uv.y*7890.12)*43758.5453)+cloudWeatherPolicy.y)
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
     let wetness=0.0
     let primaryType=cloudMorphology.x
     let primaryBody=0.5
     for(j in range(int(cloudQuality.y)+1,9)){
      let lightU=float(j)/max(cloudQuality.y,1.0)
      let lightV=max(float(j)-1.0,0.0)/max(cloudQuality.y,1.0)
      let lightDs=(lightU*lightU-lightV*lightV)*cloudLayer.y*1.5
      let p=cloudSample+L*((lightU*lightU+lightV*lightV)*cloudLayer.y*0.75)
      let h=(p.y-cloudLayer.x)/cloudLayer.y
      let density=0.0
      if((h>0.0 && h<1.0 || cloudFieldControl.x>0.5 && j==0 && h<=0.0 && p.y>0.0) && (j==0 || medium>0.0001)){
       let seedPhase=vec3(cloudFraction(cloudTint.w*0.1031),cloudFraction(cloudTint.w*0.1733),cloudFraction(cloudTint.w*0.2377))
       let q=p/cloudShape.x+cloudPhase.xyz+seedPhase
       if(cloudForm.x>3.5 && cloudForm.x<4.5){q=vec3(q.x*0.32+q.z*0.12,q.y*0.7,q.z*3.5);q.x=q.x+sin(q.z*2.0)*0.045}
       // Periodic warp: advancing any normalized phase by one remains continuous.
       q=q+vec3(sin(q.z*6.283185),sin(q.x*6.283185),sin(q.y*6.283185))*0.035
       let qd=p/cloudShape.y+cloudDetailPhase.xyz
       let z=cloudFraction(q.z)*cloudShapeLayout.x-0.5
       let zd=cloudFraction(qd.z)*cloudDetailLayout.x-0.5
       let n=mix(sampleLod(shapeNoise,cloudAtlasUv(q,floor(z),cloudShapeLayout),0.0),sampleLod(shapeNoise,cloudAtlasUv(q,floor(z)+1.0,cloudShapeLayout),0.0),cloudFraction(z))
       let d=mix(sampleLod(detailNoise,cloudAtlasUv(qd,floor(zd),cloudDetailLayout),0.0).rgb,sampleLod(detailNoise,cloudAtlasUv(qd,floor(zd)+1.0,cloudDetailLayout),0.0).rgb,cloudFraction(zd))
       // World-space macro weather is independent of the short, wrapping volume phase.
       let macro=vec2(p.x*0.93+p.z*0.37,p.z*0.93-p.x*0.37)/cloudShape.w+vec2(0.37,0.63)
       let mapUv=vec2(cloudFraction(macro.x),cloudFraction(macro.y))
       let mapInside=1.0
       if(cloudFieldControl.x>0.5){
        let mapped=(p.xz-cloudFieldBounds.xy)/cloudFieldBounds.zw
        if(cloudFieldControl.y>0.5){mapUv=vec2(cloudFraction(mapped.x),cloudFraction(mapped.y))}
        else{mapUv=clamp(mapped,cloudFieldControl.zw*0.5,vec2(1.0)-cloudFieldControl.zw*0.5);if(mapped.x<0.0 || mapped.y<0.0 || mapped.x>1.0 || mapped.y>1.0){mapInside=0.0}}
       }
       let weather=sampleLod(weatherMap,mapUv,0.0)
       let local=weather.r*cloudLayer.z
       let kind=cloudMorphology.x
       let wet=0.0
       if(cloudFieldControl.x>0.5){local=weather.r*cloudLayer.z*mapInside;kind=weather.g;wet=weather.b*mapInside;h=h/(0.25+0.75*weather.a)}
       if(j==0){wetness=wet;primaryType=kind}
       // Continuous 3D density, rather than one fixed ellipsoid family per grid cell.
       let body=clamp(n.r*0.8+n.g*0.13+n.b*0.07,0.0,1.0)
       let broad=body
       if(cloudForm.x>0.5){
        let qm=q*2.17+vec3(0.19,0.41,0.07)
        let zm=cloudFraction(qm.z)*cloudShapeLayout.x-0.5
        let middle=mix(sampleLod(shapeNoise,cloudAtlasUv(qm,floor(zm),cloudShapeLayout),0.0).r,sampleLod(shapeNoise,cloudAtlasUv(qm,floor(zm)+1.0,cloudShapeLayout),0.0).r,cloudFraction(zm))
        body=clamp(body*0.72+middle*0.28,0.0,1.0)
       }
       let layerProfile=smoothstep(0.0,0.045,h)*(1.0-smoothstep(0.45,0.75,h))
       let towerProfile=smoothstep(0.0,0.07,h)*(1.0-smoothstep(0.35+body*0.35,0.65+body*0.35,h))
       let profile=mix(layerProfile,towerProfile,kind)
       let potential=mix(0.78+body*0.22,body,kind)*profile
       if(cloudForm.x>0.5 && cloudForm.x<1.5){
        kind=1.0
        profile=smoothstep(0.0,0.065,h)*(1.0-smoothstep(0.22+broad*0.48,0.37+broad*0.55,h))
        potential=(body*0.92+broad*0.08)*profile
       }
       if(cloudForm.x>1.5 && cloudForm.x<2.5){
        kind=0.45
        profile=smoothstep(0.0,0.07,h)*(1.0-smoothstep(0.3+broad*0.28,0.5+broad*0.3,h))
        potential=(0.15+body*0.85)*profile
       }
       if(cloudForm.x>2.5 && cloudForm.x<3.5){
        kind=0.0
        profile=smoothstep(0.0,0.09,h)*(1.0-smoothstep(0.48+broad*0.12,0.72+broad*0.15,h))
        potential=(0.84+body*0.16)*profile
       }
       if(cloudForm.x>3.5 && cloudForm.x<4.5){
        kind=0.0
        profile=smoothstep(0.0,0.22,h)*(1.0-smoothstep(0.45,1.0,h))
        potential=(body*0.8+n.g*0.2)*profile
       }
       if(cloudForm.x>4.5){
        kind=1.0
        local=clamp(local-h*0.45+smoothstep(0.72,0.9,h)*0.12,0.0,1.0)
        profile=smoothstep(0.0,0.045,h)*(1.0-smoothstep(0.56+broad*0.32,0.72+broad*0.25,h))
        potential=(broad*0.65+body*0.35+smoothstep(0.65,0.85,h)*0.04)*profile
       }
       if(j==0){primaryType=kind;primaryBody=body}
       let threshold=1.0-local*(0.85+cloudMorphology.y*0.1)
       let edge=mix(0.035,0.22,cloudMorphology.z)
       let shape=smoothstep(threshold,threshold+edge,potential)*smoothstep(0.0,0.08,local)
       if(cloudForm.x>3.5 && cloudForm.x<4.5){shape=smoothstep(1.0-local,1.3-local,cloudWisps(p.xz/cloudShape.x*4.0,cloudTint.w))*profile*smoothstep(0.0,0.08,local)}
       let erosion=dot(d,vec3(0.625,0.25,0.125))*cloudShape.z*0.18*(1.0-shape)*(1.0-shape)
       let veil=(1.0-kind)*cloudMorphology.y*0.06*local*layerProfile
       if(cloudForm.x>0.5){veil=0.0}
       density=(max(shape-erosion,0.0)+veil)*smoothstep(0.001,0.1,body)*cloudLayer.w*mix(2.0,3.0,kind)*(1.0+wet*0.8)
       if(h<=0.0 && cloudFieldControl.x>0.5){density=wet*cloudWeatherPolicy.x*0.025*smoothstep(0.0,0.15,(cloudLayer.x-p.y)/max(cloudLayer.x,1.0))*(0.6+body*0.8)}
      }
      if(j==0){medium=density*(1.0-smoothstep(cloudQuality.z*0.65,cloudQuality.z,rayDistance))}else{lightDepth=lightDepth+density*lightDs}
     }
     let stepT=exp(-medium*cloudOptics.x*ds)
     let lightT=mix(1.0,exp(-lightDepth*cloudOptics.x),cloudOptics.w)
     let height=clamp((cloudSample.y-cloudLayer.x)/cloudLayer.y,0.0,1.0)
     // Two attenuated scattering orders approximate energy reaching thick cloud interiors.
     let scattered=exp(-lightDepth*cloudOptics.x*0.25)
     let multiple=mix(1.0,scattered,cloudOptics.w)
     let lighting=cloudAmbient.rgb*(mix(1.8,0.7,primaryType)+height*0.6)*(1.0-wetness*0.5*(1.0-height))+cloudLightColor.rgb*(lightT*phase*0.65+multiple*0.15)
     if(cloudForm.x>0.5){lighting=lighting*(0.82+primaryBody*0.36)}
     if(cloudForm.x>3.5 && cloudForm.x<4.5){lighting=cloudAmbient.rgb*1.8+cloudLightColor.rgb*0.7}
     let aerial=1.0-exp(-rayDistance*cloudMorphology.w)
     let lit=cloudOptics.y*mix(cloudTint.rgb*lighting,cloudHorizon.rgb,aerial)
     radiance=radiance+transmission*(1.0-stepT)*lit
     transmission=transmission*stepT
    }
   }
   }
  }
  let finalColor=background*transmission+radiance
  // Thin high clouds use a separate altitude, anisotropic structure and analytic optical path.
  // This is deliberately independent of the primary weather map and adds no second raymarch.
  if(cloudHighLayer.z>0.0 && cloudHighLayer.w>0.0 && abs(direction.y)>0.0001){
   let distance=(cloudHighLayer.x+cloudHighLayer.y*0.5-origin.y)/direction.y
   if(distance>0.0 && distance<limit){
    let p=origin+direction*distance
    let filament=smoothstep(1.0-cloudHighLayer.z,1.35-cloudHighLayer.z,cloudWisps(p.xz/cloudHighShape.x*4.0,cloudTint.w+43.0))
    let optical=filament*cloudHighLayer.w*cloudOptics.x*cloudHighLayer.y/max(abs(direction.y),0.08)*(1.0-smoothstep(cloudQuality.z*0.7,cloudQuality.z,distance))
    let highT=exp(-optical)
    let highLight=mix(cloudTint.rgb*(cloudAmbient.rgb*1.8+cloudLightColor.rgb*0.55),cloudHorizon.rgb,1.0-exp(-distance*cloudMorphology.w))
    let highR=(1.0-highT)*highLight*cloudOptics.y
    if(distance<begin){finalColor=finalColor*highT+highR}
    else{finalColor=(background*highT+highR)*transmission+radiance}
    transmission=transmission*highT
    radiance=finalColor-background*transmission
   }
  }
  let result=vec4(finalColor,raw.a)
  if(cloudQuality.w>1.5){result=vec4(radiance,raw.a)}else if(cloudQuality.w>0.5){result=vec4(vec3(transmission),raw.a)}
  if(cloudQuality.w>2.5){result=vec4(direction*0.5+vec3(0.5),1.0)}
  return result
 }
}
)PHOSKIA";
}
