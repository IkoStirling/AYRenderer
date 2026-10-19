#include <AYRenderer/EmissivePointCloud.h>
#include <algorithm>
#include <cmath>

namespace ayt::render {
bool EmissivePointLightingHistory::update(std::span<const EmissivePoint> points,
    EmissivePointLightingSettings settings,double time,float transitionSeconds,
    std::vector<PerformanceLight>& output,EmissivePointLightingStats& stats,std::string* error) {
    if(!std::isfinite(time)||time<0||!std::isfinite(transitionSeconds)||transitionSeconds<0||transitionSeconds>1
        ||!settings.smoothCells){if(error)*error="Point lighting history requires smooth cells, finite time and transition0..1s";return false;}
    std::vector<PerformanceLight> candidates;EmissivePointLightingStats next;
    if(!buildEmissivePointLights(points,settings,candidates,next,error))return false;
    const bool same=_settings.maxLights==settings.maxLights&&_settings.cellSize==settings.cellSize
        &&_settings.range==settings.range&&_settings.intensityScale==settings.intensityScale
        &&_settings.smoothCells==settings.smoothCells&&_transition==transitionSeconds
        &&_settings.contactDistance==settings.contactDistance&&_settings.contactThickness==settings.contactThickness
        &&_settings.contactBias==settings.contactBias&&_settings.contactStrength==settings.contactStrength;
    const bool restart=!_valid||!same||time<_time||time-_time>.25||transitionSeconds==0||next.contributing==0;
    auto slots=_slots;
    if(restart){slots.clear();for(auto light:candidates)slots.push_back({light,1});next.historyReset=true;}
    else {
        const float step=float((time-_time)/transitionSeconds);
        const auto matches=[](const auto& a,const auto& b){return a.light.position.x==b.light.position.x&&a.light.position.y==b.light.position.y&&a.light.position.z==b.light.position.z;};
        for(auto& slot:slots){
            const auto found=std::find_if(candidates.begin(),candidates.end(),[&](const auto& c){return matches(slot.light,c);});
            if(found!=candidates.end()){const auto id=slot.light.id;slot.light=*found;slot.light.id=id;slot.gain=std::min(1.f,slot.gain+step);}
            else slot.gain=std::max(0.f,slot.gain-step);
        }
        std::erase_if(slots,[](const auto& slot){return slot.gain<=0;});
        for(auto light:candidates){
            if(std::any_of(slots.begin(),slots.end(),[&](const auto& slot){return matches(slot.light,light);}))continue;
            if(slots.size()>=settings.maxLights){++next.deferredCells;continue;}
            uint32_t id=0;while(std::any_of(slots.begin(),slots.end(),[&](const auto& slot){return slot.light.id==id;}))++id;
            light.id=id;slots.push_back({light,0});
        }
    }
    std::vector<PerformanceLight> lights;lights.reserve(slots.size());
    for(const auto& slot:slots){if(slot.gain<1)++next.fadingLights;if(slot.gain<=0)continue;auto light=slot.light;light.light.intensity*=slot.gain;lights.push_back(light);}
    next.selectedLights=uint32_t(lights.size());output=std::move(lights);stats=next;
    _slots=std::move(slots);_settings=settings;_time=time;_transition=transitionSeconds;_valid=true;
    if(error)error->clear();return true;
}
} // namespace ayt::render
