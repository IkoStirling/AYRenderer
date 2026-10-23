#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ayt::render {
/// Optional single diffuse bounce from current-frame opaque, illuminated screen
/// receivers. Linear HDR, no feedback/history/off-screen reconstruction; default off.
struct ScreenSpaceIndirectSettings {
    bool enabled=false;
    float strength=.5f; // [0,1], artistic contribution, bounded by one diffuse bounce
    float maxDistance=10; // world units
    float thickness=.1f; // receiver tolerance and normal-origin offset
    uint8_t rays=4; // deterministic cosine-weighted hemisphere quadrature, 1..4
    uint8_t steps=24; // 4..32 per ray, four hit refinements
};
inline ScreenSpaceIndirectSettings sanitizeScreenSpaceIndirect(ScreenSpaceIndirectSettings s) noexcept {
    const auto finite=[](float x,float fallback,float lo,float hi){return std::isfinite(x)?std::clamp(x,lo,hi):fallback;};
    s.strength=finite(s.strength,.5f,0,1);s.maxDistance=finite(s.maxDistance,10,.1f,100);
    s.thickness=finite(s.thickness,.1f,.001f,2);s.rays=std::clamp<uint8_t>(s.rays,1,4);s.steps=std::clamp<uint8_t>(s.steps,4,32);return s;
}
}
