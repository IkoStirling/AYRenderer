#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <numeric>
namespace ayt::render::detail {
inline constexpr int cloudAtlasWidth=272,cloudAtlasHeight=136;
struct CloudAtlasLayout {int resolution=0,columns=0,rows=0;};
inline CloudAtlasLayout cloudAtlasLayout(int width,int height){
    for(const auto layout:{CloudAtlasLayout{32,8,4},CloudAtlasLayout{64,8,8},CloudAtlasLayout{128,16,8}})
        if(width==layout.columns*(layout.resolution+2)&&height==layout.rows*(layout.resolution+2))return layout;
    return {};
}
inline constexpr int cloudWeatherSize=256;
// A separate, much larger stationary weather field breaks up periodic volume repetitions.
// Rank remapping gives coverage a useful range rather than concentrating all values near 0.5.
inline std::vector<uint8_t> makeCloudWeather(uint32_t seed){
    constexpr int n=cloudWeatherSize;
    const auto hash=[&](int x,int y,int period){uint32_t h=seed^uint32_t((x+period)%period)*73856093u^uint32_t((y+period)%period)*19349663u;h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;h^=h>>16;return float(h&65535)/65535;};
    const auto noise=[&](int x,int y,int period){float px=float(x)*period/n,py=float(y)*period/n;int ix=int(px),iy=int(py);float tx=px-ix,ty=py-iy;tx=tx*tx*(3-2*tx);ty=ty*ty*(3-2*ty);return (hash(ix,iy,period)*(1-tx)+hash(ix+1,iy,period)*tx)*(1-ty)+(hash(ix,iy+1,period)*(1-tx)+hash(ix+1,iy+1,period)*tx)*ty;};
    std::vector<float> values(n*n);std::vector<int> indices(n*n);std::iota(indices.begin(),indices.end(),0);
    for(int y=0;y<n;++y)for(int x=0;x<n;++x)values[y*n+x]=.65f*noise(x,y,4)+.25f*noise(x,y,8)+.1f*noise(x,y,16);
    std::stable_sort(indices.begin(),indices.end(),[&](int a,int b){return values[a]<values[b];});
    std::vector<uint8_t> pixels(n*n*4);for(int rank=0;rank<n*n;++rank){const auto i=indices[rank]*4;pixels[i]=pixels[i+1]=pixels[i+2]=uint8_t(rank*255/(n*n-1));pixels[i+3]=255;}return pixels;
}
// Tileable value-noise FBM; no global RNG, files or per-frame allocations.
inline std::vector<uint8_t> makeCloudNoise(uint32_t seed){
    const auto hash=[&](int x,int y,int z,int period){uint32_t h=seed^uint32_t((x+period)%period)*73856093u^uint32_t((y+period)%period)*19349663u^uint32_t((z+period)%period)*83492791u;h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;h^=h>>16;return float(h&65535)/65535;};
    const auto noise=[&](int x,int y,int z,int period){float p[3]={float(x)*period/32,float(y)*period/32,float(z)*period/32};int a[3];float t[3];for(int k=0;k<3;++k){a[k]=int(std::floor(p[k]));t[k]=p[k]-a[k];t[k]=t[k]*t[k]*(3-2*t[k]);}float value=0;for(int k=0;k<8;++k)value+=hash(a[0]+(k&1),a[1]+((k>>1)&1),a[2]+((k>>2)&1),period)*((k&1)?t[0]:1-t[0])*((k&2)?t[1]:1-t[1])*((k&4)?t[2]:1-t[2]);return value;};
    std::vector<uint8_t> bytes(cloudAtlasWidth*cloudAtlasHeight*4);
    for(int z=0;z<32;++z)for(int y=0;y<34;++y)for(int x=0;x<34;++x){int nx=(x+31)%32,ny=(y+31)%32;float value=.55f*noise(nx,ny,z,4)+.25f*noise(nx,ny,z,8)+.13f*noise(nx,ny,z,16)+.07f*noise(nx,ny,z,32);auto i=((z/8*34+y)*cloudAtlasWidth+z%8*34+x)*4;bytes[i]=bytes[i+1]=bytes[i+2]=uint8_t(value*255);bytes[i+3]=255;}
    return bytes;
}
}
