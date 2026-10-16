#include <AYRenderer/PerformanceLighting.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace ayt::render {
bool PerformanceLighting::assign(std::span<const PerformanceLight> source, PerformanceLightBudget limits, std::string* error) {
    const auto fail=[&](const char* message){if(error)*error=message;return false;};
    if(source.size()>256 || limits.surfaceLights>256 || limits.volumeLights>64 || limits.shadowLights>8)
        return fail("Performance lighting budget exceeded");
    PerformanceLighting next; next.budget=limits;
    std::unordered_set<uint32_t> ids;
    for(const auto& entry:source) {
        const auto& l=entry.light;
        const auto finite=[](float v,float a,float b){return std::isfinite(v)&&v>=a&&v<=b;};
        if(!ids.insert(entry.id).second || l.type!=LightType::Spot || !finite(l.range,.01f,10000)
            || !finite(l.intensity,0,100000) || !finite(l.coneCosOuter,.001f,.999999f)
            || !finite(l.coneCosInner,l.coneCosOuter+.000001f,1)
            || !finite(l.shadowBias,0,1) || !finite(entry.volumetric,0,100)
            || !finite(entry.patternRotation,-100000,100000) || !finite(entry.patternScale,.01f,100)
            || !finite(entry.patternFocus,0,1) || !finite(entry.iris,.01f,1)
            || !finite(entry.apertureRadius,0,1) || !finite(entry.shadowSoftness,0,4)
            || entry.pattern>LightPattern::Texture
            || (entry.pattern==LightPattern::Texture&&!entry.goboTexture.isValid()))
            return fail("Invalid performance Spot light");
        for(int i=0;i<3;++i) if(!finite(l.position[i],-100000,100000) || !finite(l.color[i],0,10000)
            || !finite(l.spotDirection[i],-100000,100000)) return fail("Invalid light position/color/direction");
        auto copy=entry;
        const float length=l.spotDirection.length();
        if(length<.00001f) return fail("Zero Spot direction");
        copy.light.spotDirection=copy.light.spotDirection/length;
        next.lights.push_back(copy);
    }
    std::stable_sort(next.lights.begin(),next.lights.end(),[](const auto& a,const auto& b){return a.priority>b.priority;});
    next.shadowSlots.assign(next.lights.size(),-1);
    for(size_t i=0;i<next.lights.size();++i) {
        const auto& light=next.lights[i].light;
        if(light.castShadow && light.intensity>0 && next.shadowSources.count<limits.shadowLights) {
            next.shadowSlots[i]=static_cast<int32_t>(next.shadowSources.count);
            Light shadow=light;shadow.color={0,0,0};
            next.shadowSources.add(shadow);
        }
    }
    next.stats.sourceLights=static_cast<uint32_t>(next.lights.size());next.stats.shadowLights=next.shadowSources.count;
    // Suppress legacy direct-light fallback while retaining ambient/IBL exactly once.
    if(next.shadowSources.count==0)next.shadowSources.add(Light::directional({0,-1,0},{0,0,0}));
    *this=std::move(next);if(error)error->clear();return true;
}
void performanceLightBounds(const PerformanceLight& entry,math::FVector3& center,math::FVector3& halfExtent) {
    const auto& light=entry.light;const auto d=light.spotDirection;
    const auto base=light.position+d*light.range;
    const float radius=entry.apertureRadius+light.range*std::tan(std::acos(light.coneCosOuter));
    for(int i=0;i<3;++i) {
        const float extent=radius*std::sqrt(std::max(0.f,1-d[i]*d[i]));
        const float apertureExtent=entry.apertureRadius*std::sqrt(std::max(0.f,1-d[i]*d[i]));
        const float low=std::min(light.position[i]-apertureExtent,base[i]-extent);
        const float high=std::max(light.position[i]+apertureExtent,base[i]+extent);
        center[i]=(low+high)*.5f;halfExtent[i]=std::max(.0001f,(high-low)*.5f);
    }
}
LightScreenRect performanceLightRect(const PerformanceLight& entry,const math::Float4x4& vp,uint16_t width,uint16_t height) {
    if(!width||!height||entry.light.intensity<=0)return {};
    math::FVector3 p,r;performanceLightBounds(entry,p,r);
    float minX=std::numeric_limits<float>::infinity(),minY=minX,maxX=-minX,maxY=-minX;int front=0,back=0;
    for(int i=0;i<8;++i) {
        const auto c=vp*math::FVector4(p.x+(i&1?r.x:-r.x),p.y+(i&2?r.y:-r.y),p.z+(i&4?r.z:-r.z),1);
        if(!std::isfinite(c.x)||!std::isfinite(c.y)||!std::isfinite(c.w))return {0,0,width,height,true};
        if(c.w<=.00001f){++back;continue;}++front;
        const float x=c.x/c.w,y=c.y/c.w;
        if(!std::isfinite(x)||!std::isfinite(y))return {0,0,width,height,true};
        minX=std::min(minX,x);maxX=std::max(maxX,x);
        minY=std::min(minY,y);maxY=std::max(maxY,y);
    }
    if(!front)return {};
    if(back)return {0,0,width,height,true};
    if(maxX< -1||minX>1||maxY< -1||minY>1)return {};
    const int x=std::clamp(int(std::floor((std::clamp(minX,-1.f,1.f)*.5f+.5f)*width))-2,0,int(width));
    const int y=std::clamp(int(std::floor((.5f-std::clamp(maxY,-1.f,1.f)*.5f)*height))-2,0,int(height));
    const int endX=std::clamp(int(std::ceil((std::clamp(maxX,-1.f,1.f)*.5f+.5f)*width))+2,0,int(width));
    const int endY=std::clamp(int(std::ceil((.5f-std::clamp(minY,-1.f,1.f)*.5f)*height))+2,0,int(height));
    return {static_cast<uint16_t>(x),static_cast<uint16_t>(y),static_cast<uint16_t>(endX-x),static_cast<uint16_t>(endY-y),endX>x&&endY>y};
}
} // namespace ayt::render
