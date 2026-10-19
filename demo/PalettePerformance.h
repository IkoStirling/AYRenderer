#pragma once
#include "AYRenderer/ColorStylizationSettings.h"
#include <algorithm>
#include <cmath>
namespace ayt::render::demo {
// Presentation-only recipe: interpolate actual palette values. Reordering a
// nearest-color palette alone would leave the rendered set unchanged.
inline ColorStylizationSettings palettePerformance(const ColorStylizationSettings& base,float seconds) {
    if(base.colorMapping!=StylizationColorMapping::FixedPalette || !std::isfinite(seconds))return base;
    auto look=base;
    const float cycle=std::fmod(std::max(0.f,seconds)/3.2f,1.f)*3;
    const int segment=std::min(int(cycle),2);const float t=cycle-segment,ease=t*t*(3-2*t);
    for(uint8_t i=0;i<look.paletteSize;++i) {
        const auto c=base.palette[i];const float v=std::max(c.r,std::max(c.g,c.b));
        const StylizationColor bank[3]={c,{c.r*.75f+v*.25f,c.g*.72f+v*.12f,c.b*.55f},
            {c.r*.55f,c.g*.8f+v*.05f,c.b*.65f+v*.25f}};
        const auto a=bank[segment],b=bank[(segment+1)%3];
        look.palette[i]={a.r+(b.r-a.r)*ease,a.g+(b.g-a.g)*ease,a.b+(b.b-a.b)*ease};
    }
    return look;
}
}
