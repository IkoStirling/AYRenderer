#pragma once
#include "AYMath/MathTypes.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ayt::render {
/// Advanced host controls. One selected light, optional axis-aligned local medium.
struct VolumetricSettings {
    bool enabled = false;
    float density = 0.025f; // extinction, inverse world units
    float scattering = 0.8f; // single-scattering albedo, [0,1]
    float anisotropy = 0.2f;
    float maxDistance = 100.0f;
    uint8_t steps = 32;
    uint8_t lightIndex = 0;
    uint8_t debugView = 0; // 0 composite, 1 transmittance, 2 radiance
    bool local = false;
    math::FVector3 center{0,0,0};
    math::FVector3 halfExtent{10,10,10};
    math::FVector3 tint{1,1,1};
};
inline VolumetricSettings sanitizeVolumetricSettings(VolumetricSettings s) noexcept {
    auto finite=[](float x,float fallback,float lo,float hi){return std::isfinite(x)?std::clamp(x,lo,hi):fallback;};
    s.density=finite(s.density,.025f,0,1); s.scattering=finite(s.scattering,.8f,0,1);
    s.anisotropy=finite(s.anisotropy,.2f,-.9f,.9f); s.maxDistance=finite(s.maxDistance,100,.1f,500);
    s.steps=std::clamp<uint8_t>(s.steps,8,64); s.lightIndex=std::min<uint8_t>(s.lightIndex,7); s.debugView=std::min<uint8_t>(s.debugView,2);
    for(int i=0;i<3;++i) {s.center[i]=finite(s.center[i],0,-100000,100000);s.halfExtent[i]=finite(s.halfExtent[i],10,.01f,10000);s.tint[i]=finite(s.tint[i],1,0,1);}
    return s;
}
}
