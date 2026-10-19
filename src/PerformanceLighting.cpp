#include <AYRenderer/PerformanceLighting.h>
#include "detail/SpotShadowProjection.h"
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
        if(!ids.insert(entry.id).second || (l.type!=LightType::Spot&&l.type!=LightType::Point) || !finite(l.range,.01f,10000)
            || !finite(l.intensity,0,100000) || (l.type==LightType::Spot&&(!finite(l.coneCosOuter,.001f,.999999f)
            || !finite(l.coneCosInner,l.coneCosOuter+.000001f,1)))
            || !finite(l.shadowBias,0,1) || !finite(entry.volumetric,0,100)
            || !finite(entry.patternRotation,-100000,100000) || !finite(entry.patternScale,.01f,100)
            || !finite(entry.patternFocus,0,1) || !finite(entry.iris,.01f,1)
            || !finite(entry.apertureRadius,0,1) || !finite(entry.shadowSoftness,0,4)
            || (l.type==LightType::Point&&(l.castShadow||entry.volumetric!=0||entry.pattern!=LightPattern::Open||entry.constantFlux))
            || !finite(entry.areaShadowBias,0,.1f)|| !finite(entry.focusDistance,0,10000)||entry.prismCount<1||entry.prismCount>8
            || !finite(entry.prismAngle,0,.35f)||!finite(entry.prismRotation,-100000,100000)
            || (l.type==LightType::Spot&&entry.prismAngle>0&&std::acos(l.coneCosOuter)+entry.prismAngle>1.56f)
            || (l.type==LightType::Point&&(entry.prismCount!=1||entry.focusDistance!=0||entry.areaShadow))
            || entry.intensityUnit>LightIntensityUnit::Lumen
            || (bool(entry.iesProfile)!=entry.iesTexture.isValid())
            || (entry.iesProfile&&(l.type!=LightType::Spot||entry.iesProfile->peakCandela()<=0))
            || entry.pattern>LightPattern::Texture
            || (entry.pattern==LightPattern::Texture&&!entry.goboTexture.isValid()))
            return fail("Invalid performance Spot light");
        for(int i=0;i<3;++i) if(!finite(l.position[i],-100000,100000) || !finite(l.color[i],0,10000)
            || !finite(l.spotDirection[i],-100000,100000)) return fail("Invalid light position/color/direction");
        auto copy=entry;
        const float length=l.spotDirection.length();
        if(l.type==LightType::Point){copy.light.spotDirection={0,-1,0};copy.light.coneCosInner=.95f;copy.light.coneCosOuter=.8f;}
        else {
            if(length<.00001f) return fail("Zero Spot direction");
            copy.light.spotDirection=copy.light.spotDirection/length;
        }
        next.lights.push_back(copy);
    }
    std::stable_sort(next.lights.begin(),next.lights.end(),[](const auto& a,const auto& b){return a.priority>b.priority;});
    next.shadowSlots.assign(next.lights.size(),-1);
    for(size_t i=0;i<next.lights.size();++i) {
        const auto& light=next.lights[i].light;
        if(light.castShadow && light.intensity>0 && next.shadowSources.count<limits.shadowLights) {
            next.shadowSlots[i]=static_cast<int32_t>(next.shadowSources.count);
            Light shadow=light;shadow.color={0,0,0};
            if(next.lights[i].prismCount>1)shadow.coneCosOuter=std::cos(std::acos(light.coneCosOuter)+next.lights[i].prismAngle);
            if(next.lights[i].areaShadow&&next.lights[i].apertureRadius>0){
                const auto projection=detail::spotShadowProjection(shadow.coneCosOuter,shadow.range);
                const float slope=std::tan(std::acos(shadow.coneCosOuter))+next.lights[i].apertureRadius*next.lights[i].iris/projection.nearZ;
                shadow.coneCosOuter=std::cos(std::min(std::atan(slope),1.527163095f));
            }
            next.shadowSources.add(shadow);
        }
    }
    next.stats.sourceLights=static_cast<uint32_t>(next.lights.size());next.stats.shadowLights=next.shadowSources.count;
    // Suppress legacy direct-light fallback while retaining ambient/IBL exactly once.
    if(next.shadowSources.count==0)next.shadowSources.add(Light::directional({0,-1,0},{0,0,0}));
    *this=std::move(next);if(error)error->clear();return true;
}
float performanceLightFluxScale(const PerformanceLight& light) noexcept {
    if(!light.constantFlux||light.light.type!=LightType::Spot)return 1;
    // Integral of smoothstep across cosine space is exactly half its interval.
    // The common 2*pi factor cancels. Double avoids narrow-cone cancellation.
    constexpr double reference=2.0-0.9993908270190958-0.9986295347545738;
    const double solidAngle=2.0-double(light.light.coneCosInner)-double(light.light.coneCosOuter);
    return float(reference/std::max(solidAngle,1e-9));
}
float performanceShadowPenumbra(float radius,float receiver,float blocker) noexcept {
    if(!std::isfinite(radius)||!std::isfinite(receiver)||!std::isfinite(blocker)||radius<0||blocker<=0||receiver<=blocker)return 0;
    return float(std::min(double(radius)*(receiver-blocker)/blocker,1e30));
}
float performanceLightIntensity(const PerformanceLight& entry) noexcept {
    if(entry.intensityUnit==LightIntensityUnit::Legacy)return entry.light.intensity*performanceLightFluxScale(entry);
    const auto& l=entry.light;const double luminance=.2126*l.color.x+.7152*l.color.y+.0722*l.color.z;
    if(luminance<=0)return 0;
    double scale=1;
    if(entry.intensityUnit==LightIntensityUnit::Lumen){
        scale=l.type==LightType::Point?12.566370614359172:entry.iesProfile?entry.iesProfile->coneSolidAngle(l.coneCosInner,l.coneCosOuter):spotSolidAngle(l.coneCosInner,l.coneCosOuter);
        if(scale<=0)return 0;
    }
    return float(std::min(double(l.intensity)/(scale*luminance),1e30));
}
math::FVector3 performanceLightLobeDirection(const PerformanceLight& entry,uint8_t index) noexcept {
    const auto d=entry.light.spotDirection;if(entry.prismCount<=1||entry.prismAngle==0)return d;
    const auto a=std::abs(d.y)>.99f?math::FVector3(0,0,1):math::FVector3(0,1,0),right=a.cross(d).normalize(),up=d.cross(right).normalize();
    const float angle=entry.prismRotation+float(index)*6.283185307179586f/entry.prismCount;
    return d*std::cos(entry.prismAngle)+(right*std::cos(angle)+up*std::sin(angle))*std::sin(entry.prismAngle);
}
float performanceLightFocusBlur(const PerformanceLight& entry,float distance) noexcept {
    if(entry.focusDistance<=0||!std::isfinite(distance)||distance<0)return 0;
    const double pupil=double(entry.apertureRadius)*entry.iris;
    const double beam=entry.apertureRadius+double(distance)*std::tan(std::acos(entry.light.coneCosOuter));
    return float(std::min(2.0,pupil*std::abs(1-double(distance)/entry.focusDistance)/std::max(beam,1e-9)));
}
float performanceLightAngular(const PerformanceLight& entry,const math::FVector3& ray,float glow) noexcept {
    float value=0;const auto& l=entry.light;
    for(uint8_t i=0;i<entry.prismCount;++i){const auto d=performanceLightLobeDirection(entry,i);const float facing=d.dot(ray);
        const float t=std::clamp((facing-l.coneCosOuter)/(l.coneCosInner-l.coneCosOuter),0.f,1.f),beam=t*t*(3-2*t);
        float ies=1;if(entry.iesProfile){const auto a=std::abs(d.y)>.99f?math::FVector3(0,0,1):math::FVector3(0,1,0),right=a.cross(d).normalize(),up=d.cross(right).normalize();
            ies=entry.iesProfile->sample(float(std::acos(std::clamp(facing,-1.f,1.f))*57.29577951308232),float(std::atan2(up.dot(ray),right.dot(ray))*57.29577951308232))/entry.iesProfile->peakCandela();}
        value+=(beam+(1-beam)*glow*std::max(facing,0.f))*ies;
    }
    return value/entry.prismCount;
}
void performanceLightBounds(const PerformanceLight& entry,math::FVector3& center,math::FVector3& halfExtent) {
    const auto& light=entry.light;
    if(light.type==LightType::Point){center=light.position;halfExtent={light.range,light.range,light.range};return;}
    math::FVector3 low=light.position,high=light.position;
    const float radius=entry.apertureRadius+light.range*std::tan(std::acos(light.coneCosOuter));
    for(uint8_t j=0;j<entry.prismCount;++j){const auto d=performanceLightLobeDirection(entry,j),base=light.position+d*light.range;
        for(int i=0;i<3;++i){const float disk=std::sqrt(std::max(0.f,1-d[i]*d[i])),extent=radius*disk,aperture=entry.apertureRadius*disk;low[i]=std::min({low[i],base[i]-extent,light.position[i]-aperture});high[i]=std::max({high[i],base[i]+extent,light.position[i]+aperture});}}
    center=(low+high)*.5f;halfExtent=(high-low)*.5f;
    for(int i=0;i<3;++i)halfExtent[i]=std::max(.0001f,halfExtent[i]);
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
