#pragma once
namespace ayt::render {
// Shared by ray integration, froxel density injection and full-resolution extinction.
inline constexpr const char* kVolumetricDensityFunctions=R"(
fn volumetricGridSample(atlas:sampler2D, local:vec3, dims:vec4):float {
 if(max(max(abs(local.x),abs(local.y)),abs(local.z))>1.0){return 0.0}
 let uv=vec3(local*0.5+vec3(0.5))
 let z=uv.z*(dims.z-1.0)
 let first=floor(z);let second=min(first+1.0,dims.z-1.0)
 let rows=ceil(dims.z/8.0)
 let firstRow=floor(first/8.0);let secondRow=floor(second/8.0)
 let x=uv.x*(dims.x-1.0)+0.5;let y=uv.y*(dims.y-1.0)+0.5
 let a=sampleLod(atlas,vec2(((first-firstRow*8.0)*dims.x+x)/(dims.x*8.0),(firstRow*dims.y+y)/(dims.y*rows)),0.0).r
 let b=sampleLod(atlas,vec2(((second-secondRow*8.0)*dims.x+x)/(dims.x*8.0),(secondRow*dims.y+y)/(dims.y*rows)),0.0).r
 return a+(b-a)*(z-first)
}
fn volumetricMediumWeight(atlas:sampler2D, world:vec3, center:vec3, extent:vec3, options:vec4, noise:vec4, dims:vec4):float {
 let weight=1.0
 let normalized=vec3((world-center)/extent)
 if(options.z>0.5){weight=1.0-smoothstep(0.75,1.0,max(max(abs(normalized.x),abs(normalized.y)),abs(normalized.z)))}
 if(noise.x>0.001){
  let p=world*noise.y+vec3(noise.z*0.17,noise.z*0.11,noise.z*0.14)
  weight=weight*clamp(1.0+noise.x*(0.5*sin(p.x+p.z*0.73)+0.35*sin(p.y*1.7-p.z*1.1)+0.15*sin(p.x*1.3+p.y*0.6)),0.15,1.85)
 }
 if(dims.w>0.5){weight=weight*volumetricGridSample(atlas,normalized,dims)}
 return weight
}
)";
}
