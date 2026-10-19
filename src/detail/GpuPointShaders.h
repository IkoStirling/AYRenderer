#pragma once
namespace ayt::render::detail::pointgpu {
// Parameters: source/minimumPixels/projectionScale/preserveEnergy;
// absProjectionX/absProjectionY/tileCount/diagnostics; right; up.
inline constexpr const char* mark=R"(
#include <bgfx_compute.sh>
BUFFER_RO(sourcePoints, vec4, 0);
BUFFER_WO(visibleFlags, uint, 1);
uniform vec4 u_epParams[4];
uniform mat4 u_epViewProjection;
NUM_THREADS(64, 1, 1)
void main() {
 uint id=gl_GlobalInvocationID.x;
 if(id>=uint(u_epParams[0].x)) return;
 vec4 p=sourcePoints[id*2u];vec4 color=sourcePoints[id*2u+1u];
 vec4 clip=mul(u_epViewProjection,vec4(p.xyz,1.0));
 uint visible=0u;
 if(clip.w>0.00001 && color.w>0.0) {
  float radius=max(p.w,u_epParams[0].y*clip.w/u_epParams[0].z);
  vec2 margin=radius*u_epParams[1].xy;
  if(clip.x>=-clip.w-margin.x && clip.x<=clip.w+margin.x && clip.y>=-clip.w-margin.y && clip.y<=clip.w+margin.y) visible=1u;
 }
 visibleFlags[id]=visible;
}
)";
inline constexpr const char* count=R"(
#include <bgfx_compute.sh>
BUFFER_RO(visibleFlags, uint, 1);
BUFFER_WO(groupCounts, uint, 2);
uniform vec4 u_epParams[4];
NUM_THREADS(64, 1, 1)
void main() {
 uint group=gl_GlobalInvocationID.x;
 if(group>=uint(u_epParams[1].z)) return;
 uint total=0u;uint begin=group*256u;
 for(uint lane=0u;lane<256u;++lane) if(begin+lane<uint(u_epParams[0].x)) total+=visibleFlags[begin+lane];
 groupCounts[group]=total;
}
)";
inline constexpr const char* prefix=R"(
#include <bgfx_compute.sh>
BUFFER_WO(groupOffsets, uint, 1);
BUFFER_RO(groupCounts, uint, 2);
BUFFER_RW(indirectArgs, uvec4, 3);
IMAGE2D_WO(outputCount, rgba32f, 4);
uniform vec4 u_epParams[4];
NUM_THREADS(1, 1, 1)
void main() {
 uint total=0u;
 for(uint group=0u;group<uint(u_epParams[1].z);++group) {groupOffsets[group]=total;total+=groupCounts[group];}
 drawIndexedIndirect(indirectArgs,0u,6u,total,0u,0u,0u);
 if(u_epParams[1].w>0.5) imageStore(outputCount,ivec2(0,0),vec4(float(total),u_epParams[0].x,0.0,1.0));
}
)";
inline constexpr const char* compact=R"(
#include <bgfx_compute.sh>
BUFFER_RO(sourcePoints, vec4, 0);
BUFFER_RO(visibleFlags, uint, 1);
BUFFER_RO(groupOffsets, uint, 2);
BUFFER_WO(instances, vec4, 3);
uniform vec4 u_epParams[4];
uniform mat4 u_epViewProjection;
NUM_THREADS(64, 1, 1)
void main() {
 uint id=gl_GlobalInvocationID.x;
 if(id>=uint(u_epParams[0].x)) return;
 if(visibleFlags[id]==0u) return;
 uint tile=id/256u;uint lane=id-tile*256u;uint local=0u;
 for(uint j=0u;j<lane;++j) local+=visibleFlags[tile*256u+j];
 uint destination=(groupOffsets[tile]+local)*4u;
 vec4 p=sourcePoints[id*2u];vec4 color=sourcePoints[id*2u+1u];
 vec4 clip=mul(u_epViewProjection,vec4(p.xyz,1.0));
 float pixelRadius=p.w*u_epParams[0].z/clip.w;
 float radius=max(p.w,u_epParams[0].y*clip.w/u_epParams[0].z);
 float energy=1.0;
 if(u_epParams[0].w>0.5) energy=min(1.0,pixelRadius*pixelRadius/max(0.000001,u_epParams[0].y*u_epParams[0].y));
 instances[destination]=vec4(p.xyz,radius);
 instances[destination+1u]=vec4(u_epParams[2].xyz,0.0);
 instances[destination+2u]=vec4(u_epParams[3].xyz,0.0);
 instances[destination+3u]=vec4(color.xyz*(color.w*energy),1.0);
}
)";
inline constexpr const char* varyings=R"(
vec3 a_position : POSITION;
vec2 a_texcoord0 : TEXCOORD0;
vec4 i_data0 : TEXCOORD7;
vec4 i_data1 : TEXCOORD6;
vec4 i_data2 : TEXCOORD5;
vec4 i_data3 : TEXCOORD4;
vec2 v_epUv : TEXCOORD0;
vec4 v_epColor : COLOR0;
)";
inline constexpr const char* vertex=R"(
$input a_position, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_epUv, v_epColor
#include <bgfx_shader.sh>
void main() {
 vec3 offset=i_data1.xyz*(a_position.x*2.0*i_data0.w)+i_data2.xyz*(a_position.y*2.0*i_data0.w);
 gl_Position=mul(u_modelViewProj,vec4(i_data0.xyz+offset,1.0));
 v_epUv=a_texcoord0;v_epColor=i_data3;
}
)";
inline constexpr const char* fragment=R"(
$input v_epUv, v_epColor
#include <bgfx_shader.sh>
SAMPLER2D(s_epDisc,0);
void main(){gl_FragColor=texture2D(s_epDisc,v_epUv)*v_epColor;}
)";
} // namespace ayt::render::detail::pointgpu
