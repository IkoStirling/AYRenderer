#pragma once
namespace ayt::render {
inline constexpr char kDeferredDecalCacheKey[]="deferred_decal_v2_receiver_tangent";
inline constexpr char kDeferredDecalPhoskiaSource[]=R"PHOSKIA(
material DeferredDecal {
 texture2d sourceAlbedo
 texture2d sourceNormal
 texture2d sourceWorld
 texture2d sourceSurface
 texture2d decalAlbedo
 texture2d decalNormal
 uniform mat4 decalWorldToLocal
 uniform vec4 decalTangent
 uniform vec4 decalBitangent
 uniform vec4 decalFacing
 uniform vec4 decalTint
 uniform vec4 decalMaterial
 uniform vec4 decalOptions
 vertex { in pos:position; out vUv:texcoord=pos.xy*vec2(0.5)+vec2(0.5); return vec4(pos.xy,0.0,1.0) }
 fragment {
  in vUv:texcoord
  out outAlbedo:color=vec4(0.0)
  out outNormal:color=vec4(0.0)
  out outWorld:color=vec4(0.0)
  out outSurface:color=vec4(0.0)
  let uv=vec2(vUv.x,1.0-vUv.y)
  let a=sample(sourceAlbedo,uv)
  let n=sample(sourceNormal,uv)
  let w=sample(sourceWorld,uv)
  let e=sample(sourceSurface,uv)
  let local=(decalWorldToLocal*vec4(w.xyz,1.0)).xyz
  let N=normalize(n.xyz*2.0-vec3(1.0))
  let amount=0.0
  let decalUv=vec2(local.x*0.5+0.5,0.5-local.y*0.5)
  let tint=decalTint.rgb
  if(e.a>0.5 && abs(local.x)<=1.0 && abs(local.y)<=1.0 && abs(local.z)<=1.0 && dot(N,decalFacing.xyz)>=decalFacing.w) {
   amount=decalTint.a
   if(decalOptions.x>0.5){let t=sampleLod(decalAlbedo,decalUv,0.0);tint=t.rgb*decalTint.rgb;amount=amount*t.a}
  }
  outAlbedo=vec4(mix(a.rgb,tint,amount),mix(a.a,decalMaterial.y,amount*decalMaterial.z))
  let blendedNormal=N
  if(decalOptions.y>0.5 && amount>0.0 && decalMaterial.w>0.0) {
   let mapped=normalize(sampleLod(decalNormal,decalUv,0.0).xyz*2.0-vec3(1.0))
   // Neutral normal map must preserve the actual receiver normal, not flatten
   // it to the projector plane. Tangent perturbation is receiver-relative.
   let projectedT=decalTangent.xyz-N*dot(N,decalTangent.xyz)
   if(length(projectedT)<0.0001){projectedT=cross(decalBitangent.xyz,N)}
   let T=normalize(projectedT)
   let B=normalize(cross(N,T))
   let perturbed=normalize(N*max(mapped.z,0.001)+T*mapped.x+B*mapped.y)
   blendedNormal=normalize(mix(N,perturbed,amount*decalMaterial.w))
  }
  let encoded=n.xyz
  if(amount>0.0 && decalOptions.y>0.5 && decalMaterial.w>0.0){encoded=blendedNormal*0.5+vec3(0.5)}
  outNormal=vec4(encoded,mix(n.a,decalMaterial.x,amount*decalMaterial.z))
  outWorld=w
  outSurface=e
 }
}
)PHOSKIA";
}
