#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
namespace ayt::render::detail {
inline constexpr int cloudAtlasWidth=272,cloudAtlasHeight=136;
// Tileable value-noise FBM; no global RNG, files or per-frame allocations.
inline std::vector<uint8_t> makeCloudNoise(uint32_t seed){
    const auto hash=[&](int x,int y,int z,int period){uint32_t h=seed^uint32_t((x+period)%period)*73856093u^uint32_t((y+period)%period)*19349663u^uint32_t((z+period)%period)*83492791u;h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;h^=h>>16;return float(h&65535)/65535;};
    const auto noise=[&](int x,int y,int z,int period){float p[3]={float(x)*period/32,float(y)*period/32,float(z)*period/32};int a[3];float t[3];for(int k=0;k<3;++k){a[k]=int(std::floor(p[k]));t[k]=p[k]-a[k];t[k]=t[k]*t[k]*(3-2*t[k]);}float value=0;for(int k=0;k<8;++k)value+=hash(a[0]+(k&1),a[1]+((k>>1)&1),a[2]+((k>>2)&1),period)*((k&1)?t[0]:1-t[0])*((k&2)?t[1]:1-t[1])*((k&4)?t[2]:1-t[2]);return value;};
    std::vector<uint8_t> bytes(cloudAtlasWidth*cloudAtlasHeight*4);
    for(int z=0;z<32;++z)for(int y=0;y<34;++y)for(int x=0;x<34;++x){int nx=(x+31)%32,ny=(y+31)%32;float value=.55f*noise(nx,ny,z,4)+.25f*noise(nx,ny,z,8)+.13f*noise(nx,ny,z,16)+.07f*noise(nx,ny,z,32);auto i=((z/8*34+y)*cloudAtlasWidth+z%8*34+x)*4;bytes[i]=bytes[i+1]=bytes[i+2]=uint8_t(value*255);bytes[i+3]=255;}
    return bytes;
}
}
