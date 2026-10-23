#pragma once
#include "AYRenderer/RenderTypes.h"
#include <algorithm>
#include <cmath>
namespace ayt::render {
/// Main Deferred view only. No temporal reconstruction; costs scale with steps * (lightSteps+1).
struct CloudQuality { uint8_t steps=48,lightSteps=4,debugView=0;float maxDistance=60000; };
/// Single world-Y cloud slab. Heights/scales in metres, extinction in 1/m, linear RGB.
/// phase is added to world/scale. Host advances phase, Renderer never advances time.
/// Optional borrowed linear noise atlases: 32^3 R values, 8x4 tiles of 34x34 with one wrapped border
/// (272x136). Weather map is linear 2D R coverage modulation. Empty handles use seeded defaults.
/// Keep supplied textures alive through endFrame; no ownership or asset loading is transferred.
struct CloudSettings {
    bool enabled=false;
    float coverage=0,density=.35f,baseHeight=1500,thickness=2500,shapeScale=4000,detailScale=200,erosion=.35f;
    math::FVector3 shapePhase{0,0,0},detailPhase{0,0,0},tint{1,1,1};
    math::FVector3 ambientColor{.2f,.25f,.35f};
    float absorption=.0015f,scattering=.9f,anisotropy=.5f,shadowStrength=.7f;
    uint32_t seed=1;
    CloudQuality quality;
    TextureHandle shapeNoise{},detailNoise{},weatherMap{};
};
inline CloudSettings sanitizeCloudSettings(CloudSettings s) noexcept {
    auto f=[](float v,float fallback,float low,float high){return std::isfinite(v)?std::clamp(v,low,high):fallback;};
    s.coverage=f(s.coverage,0,0,1);s.density=f(s.density,.35f,0,1);s.baseHeight=f(s.baseHeight,1500,-10000,100000);
    s.thickness=f(s.thickness,2500,1,50000);s.shapeScale=f(s.shapeScale,4000,1,1000000);s.detailScale=f(s.detailScale,200,.01f,100000);
    s.erosion=f(s.erosion,.35f,0,1);s.absorption=f(s.absorption,.0015f,0,1);s.scattering=f(s.scattering,.9f,0,1);
    s.anisotropy=f(s.anisotropy,.5f,-.9f,.9f);s.shadowStrength=f(s.shadowStrength,.7f,0,1);
    for(int i=0;i<3;++i){s.tint[i]=f(s.tint[i],1,0,16);s.ambientColor[i]=f(s.ambientColor[i],.2f,0,16);s.shapePhase[i]=f(s.shapePhase[i],0,0,1);s.detailPhase[i]=f(s.detailPhase[i],0,0,1);}
    s.seed=std::max(s.seed,1u);s.quality.steps=std::clamp<uint8_t>(s.quality.steps,8,64);s.quality.lightSteps=std::min<uint8_t>(s.quality.lightSteps,8);
    s.quality.debugView=std::min<uint8_t>(s.quality.debugView,3);s.quality.maxDistance=f(s.quality.maxDistance,60000,1,200000);
    return s;
}
}
