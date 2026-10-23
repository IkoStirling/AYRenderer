#include "detail/GpuPointState.h"
#include <AYRenderer/EmissivePointCloud.h>
#include <AYRenderer.h>
#include <bgfx/bgfx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <utility>
#include <unordered_map>
#include <numeric>
#include <cstring>

namespace ayt::render {
void EmissivePointCloud::releaseResources(Renderer& renderer){
    if(_renderer!=&renderer)return;
    _gpuState.reset();_batches.clear();
    renderer.destroyMesh(_quad);
    renderer.destroyTexture(_texture);
    renderer.destroyMaterial(_material);
    renderer.destroyMaterial(_instancedMaterial);
    _renderer=nullptr;_stats={};
}

namespace {
struct VisiblePoint { std::size_t index; float radius,energy; std::uint32_t rank; };
struct PointSelection {
    std::vector<VisiblePoint> visible;
    math::FVector3 right,up;
    EmissivePointStats stats;
};
constexpr std::size_t kPointsPerBatch=16383;

bool validatePointInput(std::span<const EmissivePoint> points,const math::Float4x4& view,
    const math::Float4x4& projection,uint16_t width,uint16_t height,EmissivePointSettings settings,
    PointSelection& output,std::string* error) {
    const auto fail=[&](const char* text){if(error)*error=text;return false;};
    if(points.size()>65536||!width||!height||settings.maxPoints>65536
        ||!std::isfinite(settings.minimumPixelRadius)||settings.minimumPixelRadius<0||settings.minimumPixelRadius>16)
        return fail("Invalid point cloud viewport/budget/settings");
    for(int i=0;i<16;++i)if(!std::isfinite(view.ptr()[i])||!std::isfinite(projection.ptr()[i]))return fail("Invalid point cloud camera");
    for(const auto& p:points) {
        for(int i=0;i<3;++i)if(!std::isfinite(p.position[i])||std::abs(p.position[i])>100000
            ||!std::isfinite(p.color[i])||p.color[i]<0||p.color[i]>10000)return fail("Invalid point position/color");
        if(!std::isfinite(p.intensity)||p.intensity<0||p.intensity>100000||!std::isfinite(p.radius)||p.radius<=0||p.radius>100)
            return fail("Invalid point intensity/radius");
    }
    const auto inverse=view.inverse();
    for(int i=0;i<16;++i)if(!std::isfinite(inverse.ptr()[i]))return fail("Singular point cloud camera");
    output.right={inverse(0,0),inverse(1,0),inverse(2,0)};
    output.up={inverse(0,1),inverse(1,1),inverse(2,1)};
    const float scale=std::abs(projection(1,1))*height*.5f;
    if(scale<.00001f)return fail("Degenerate point cloud projection");
    return true;
}

bool selectVisiblePoints(std::span<const EmissivePoint> points,const math::Float4x4& view,
    const math::Float4x4& projection,uint16_t width,uint16_t height,EmissivePointSettings settings,
    PointSelection& output,std::string* error) {
    if(!validatePointInput(points,view,projection,width,height,settings,output,error))return false;
    const auto vp=projection*view;const float scale=std::abs(projection(1,1))*height*.5f;
    EmissivePointStats next;next.source=static_cast<uint32_t>(points.size());
    std::vector<VisiblePoint> visible;visible.reserve(points.size());
    for(std::size_t index=0;index<points.size();++index) {
        const auto& p=points[index];
        const auto clip=vp*math::FVector4(p.position.x,p.position.y,p.position.z,1);
        if(clip.w<=.00001f||p.intensity==0){++next.culled;continue;}
        const float pixelRadius=p.radius*scale/clip.w;
        const float radius=std::max(p.radius,settings.minimumPixelRadius*clip.w/scale);
        const float marginX=radius*std::abs(projection(0,0)),marginY=radius*std::abs(projection(1,1));
        if(clip.x< -clip.w-marginX||clip.x>clip.w+marginX||clip.y< -clip.w-marginY||clip.y>clip.w+marginY){++next.culled;continue;}
        const float energy=settings.preserveEnergy?std::min(1.f,pixelRadius*pixelRadius/std::max(.000001f,settings.minimumPixelRadius*settings.minimumPixelRadius)):1.f;
        // Stable ID rank avoids a camera-dependent first-N wedge and frame
        // flicker when a quality budget samples a dense formation.
        std::uint32_t rank=p.id;
        rank^=rank>>16;rank*=0x7feb352du;rank^=rank>>15;rank*=0x846ca68bu;rank^=rank>>16;
        visible.push_back({index,radius,energy,rank});
    }
    if(visible.size()>settings.maxPoints) {
        const auto less=[](const VisiblePoint& a,const VisiblePoint& b){
            return a.rank<b.rank||(a.rank==b.rank&&a.index<b.index);
        };
        if(settings.maxPoints)
            std::nth_element(visible.begin(),visible.begin()+settings.maxPoints,visible.end(),less);
        next.budgetSkipped=static_cast<uint32_t>(visible.size()-settings.maxPoints);
        visible.resize(settings.maxPoints);
        std::sort(visible.begin(),visible.end(),[](const auto& a,const auto& b){return a.index<b.index;});
    }
    output.visible=std::move(visible);output.stats=next;
    if(error)error->clear();return true;
}
} // namespace

bool buildEmissivePointLights(std::span<const EmissivePoint> points,
    EmissivePointLightingSettings settings,std::vector<PerformanceLight>& output,
    EmissivePointLightingStats& stats,std::string* error) {
    const auto fail=[&](const char* message){if(error)*error=message;return false;};
    const auto finite=[](float v,float lo,float hi){return std::isfinite(v)&&v>=lo&&v<=hi;};
    if(points.size()>65536||settings.maxLights>64||!finite(settings.cellSize,.01f,10000)
        ||!finite(settings.range,.01f,1000)||!finite(settings.intensityScale,0,100)
        ||!finite(settings.contactDistance,0,10)||!finite(settings.contactThickness,.001f,5)
        ||!finite(settings.contactBias,0,.5f)||!finite(settings.contactStrength,0,1))
        return fail("Invalid point lighting settings/budget");
    bool canonical=true;
    for(size_t i=0;i<points.size();++i){
        const auto& p=points[i];
        if(i&&points[i-1].id>=p.id)canonical=false;
        for(int j=0;j<3;++j)if(!finite(p.position[j],-100000,100000)||!finite(p.color[j],0,10000))
            return fail("Invalid point lighting position/color");
        if(!finite(p.intensity,0,100000)||(!std::isfinite(p.radius)||p.radius<=0||p.radius>100))return fail("Invalid point lighting intensity/radius");
    }
    // Compiled shows already use increasing stable IDs. Arbitrary caller order
    // is canonicalized only when necessary, retaining identical double sums.
    std::vector<size_t> order;
    if(!canonical){
        order.resize(points.size());std::iota(order.begin(),order.end(),size_t(0));
        std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return points[a].id<points[b].id;});
        for(size_t i=1;i<order.size();++i)if(points[order[i-1]].id==points[order[i]].id)return fail("Duplicate point lighting ID");
    }
    struct Key {
        int x,y,z;
        bool operator==(const Key&) const=default;
        bool operator<(const Key& b) const {if(x!=b.x)return x<b.x;if(y!=b.y)return y<b.y;return z<b.z;}
    };
    struct Hash {size_t operator()(const Key& k) const noexcept {
        return size_t(uint32_t(k.x))*73856093u^size_t(uint32_t(k.y))*19349663u^size_t(uint32_t(k.z))*83492791u;
    }};
    struct Cell {Key key;double rgb[3]{},center[3]{},energy=0;math::FVector3 position;float reach=0;};
    std::vector<Cell> cells;cells.reserve(std::min<size_t>(points.size(),64));
    std::unordered_map<Key,size_t,Hash> grid;grid.reserve(std::min<size_t>(points.size(),1024));
    using Neighborhood=std::array<size_t,8>;
    std::unordered_map<Key,Neighborhood,Hash> neighborhoods;
    if(settings.smoothCells)neighborhoods.reserve(std::min<size_t>(points.size(),1024));
    std::vector<size_t> membership(points.size(),size_t(-1));
    EmissivePointLightingStats next;next.source=uint32_t(points.size());
    // Merge truncated world-edge nodes even when float multiplication rounds
    // the last interior node exactly onto the boundary.
    const int edgeHigh=int(std::floor(100000.0/settings.cellSize));
    const int edgeLow=int(std::ceil(-100000.0/settings.cellSize));
    const int maxNode=edgeHigh*settings.cellSize>=100000.f?edgeHigh:edgeHigh+1;
    const int minNode=edgeLow*settings.cellSize<=-100000.f?edgeLow:edgeLow-1;
    for(size_t n=0;n<points.size();++n){
        const size_t i=canonical?n:order[n];const auto& p=points[i];
        const double weight=(.2126*p.color.x+.7152*p.color.y+.0722*p.color.z)*p.intensity*settings.intensityScale;
        if(weight<=0)continue;
        ++next.contributing;
        double gx=0,gy=0,gz=0;Key base;
        if(settings.smoothCells){
            gx=double(p.position.x)/settings.cellSize;gy=double(p.position.y)/settings.cellSize;gz=double(p.position.z)/settings.cellSize;
            base={int(std::floor(gx)),int(std::floor(gy)),int(std::floor(gz))};
        }else{
            // Preserve the original float-division cell classification exactly.
            base={int(std::floor(p.position.x/settings.cellSize)),int(std::floor(p.position.y/settings.cellSize)),int(std::floor(p.position.z/settings.cellSize))};
        }
        const double fractions[3]{gx-base.x,gy-base.y,gz-base.z};
        const double rgb[3]{double(p.color.x)*p.intensity*settings.intensityScale,double(p.color.y)*p.intensity*settings.intensityScale,double(p.color.z)*p.intensity*settings.intensityScale};
        Neighborhood* neighborhood=nullptr;
        if(settings.smoothCells){
            const auto [it,inserted]=neighborhoods.try_emplace(base);
            if(inserted)it->second.fill(size_t(-1));neighborhood=&it->second;
        }
        for(int corner=0;corner<(settings.smoothCells?8:1);++corner){
            double share=1;
            if(settings.smoothCells)for(int j=0;j<3;++j)share*=((corner>>j)&1)?fractions[j]:1-fractions[j];
            if(share<=0)continue;
            size_t cellIndex=neighborhood?(*neighborhood)[corner]:size_t(-1);
            if(cellIndex==size_t(-1)){
                Key key=base;
                if(settings.smoothCells){
                    key.x=std::clamp(key.x+(corner&1),minNode,maxNode);key.y=std::clamp(key.y+((corner>>1)&1),minNode,maxNode);key.z=std::clamp(key.z+((corner>>2)&1),minNode,maxNode);
                }
                const auto [it,inserted]=grid.try_emplace(key,cells.size());
                if(inserted)cells.push_back(Cell{key});cellIndex=it->second;
                if(neighborhood)(*neighborhood)[corner]=cellIndex;
            }
            if(!settings.smoothCells)membership[i]=cellIndex;
            auto& cell=cells[cellIndex];cell.energy+=weight*share;
            for(int j=0;j<3;++j){cell.rgb[j]+=rgb[j]*share;if(!settings.smoothCells)cell.center[j]+=p.position[j]*weight;}
        }
    }
    for(auto& cell:cells){
        if(settings.smoothCells){
            cell.position={std::clamp(cell.key.x*settings.cellSize,-100000.f,100000.f),std::clamp(cell.key.y*settings.cellSize,-100000.f,100000.f),std::clamp(cell.key.z*settings.cellSize,-100000.f,100000.f)};
            cell.reach=settings.range+std::sqrt(3.f)*settings.cellSize;
        }else{cell.position={float(cell.center[0]/cell.energy),float(cell.center[1]/cell.energy),float(cell.center[2]/cell.energy)};cell.reach=settings.range;}
    }
    for(size_t i=0;i<points.size();++i)if(membership[i]!=size_t(-1)){
        auto& cell=cells[membership[i]];cell.reach=std::max(cell.reach,settings.range+(points[i].position-cell.position).length());
    }
    struct Candidate {PerformanceLight light;double energy;Key key;};
    const auto best=[](const Candidate& a,const Candidate& b){return a.energy>b.energy||(a.energy==b.energy&&a.key<b.key);};
    std::vector<Candidate> selected;selected.reserve(settings.maxLights);
    for(auto& cell:cells){
        bool clamped=false;for(auto& c:cell.rgb){clamped|=c>1e9;c=std::min(c,1e9);}next.clampedCells+=clamped?1:0;
        const float intensity=float(std::clamp(std::max({cell.rgb[0],cell.rgb[1],cell.rgb[2]}),1e-30,100000.0));
        PerformanceLight light;light.volumetric=0;
        light.pointContactDistance=settings.contactDistance;light.pointContactThickness=settings.contactThickness;
        light.pointContactBias=settings.contactBias;light.pointContactStrength=settings.contactStrength;
        light.light=Light::point(cell.position,std::min(cell.reach,10000.f),intensity,
            {float(cell.rgb[0]/intensity),float(cell.rgb[1]/intensity),float(cell.rgb[2]/intensity)});
        Candidate candidate{light,cell.energy,cell.key};
        if(selected.size()<settings.maxLights){selected.push_back(candidate);std::push_heap(selected.begin(),selected.end(),best);}
        else if(!selected.empty()&&best(candidate,selected.front())){
            std::pop_heap(selected.begin(),selected.end(),best);selected.back()=candidate;std::push_heap(selected.begin(),selected.end(),best);
        }
    }
    next.occupiedCells=uint32_t(cells.size());next.budgetSkipped=next.occupiedCells-uint32_t(selected.size());
    std::sort(selected.begin(),selected.end(),[](const auto& a,const auto& b){return a.key<b.key;});
    std::vector<PerformanceLight> lights;lights.reserve(selected.size());
    for(auto& cell:selected){cell.light.id=uint32_t(lights.size());lights.push_back(cell.light);}
    next.selectedLights=uint32_t(lights.size());output=std::move(lights);stats=next;
    if(error)error->clear();return true;
}

bool buildEmissivePointBatches(std::span<const EmissivePoint> points,const math::Float4x4& view,
    const math::Float4x4& projection,uint16_t width,uint16_t height,EmissivePointSettings settings,
    std::vector<ParticleDrawData>& batches,EmissivePointStats& stats,std::string* error) {
    PointSelection selection;
    if(!selectVisiblePoints(points,view,projection,width,height,settings,selection,error))return false;
    const auto& visible=selection.visible;
    const auto& right=selection.right;const auto& up=selection.up;
    auto next=selection.stats;
    const size_t needed=(visible.size()+kPointsPerBatch-1)/kPointsPerBatch;
    batches.resize(needed);
    for(std::size_t i=0;i<needed;++i){
        auto& b=batches[i];const auto count=std::min(kPointsPerBatch,visible.size()-i*kPointsPerBatch);
        b.vertices.clear();b.vertices.reserve(count*4);b.indices.clear();b.indices.reserve(count*6);
        b.instanceData.clear();b.instanceStride=0;b.world3D=true;b.gpuStream=nullptr;b.submitGpu=nullptr;
    }
    for(const auto& selected:visible) {
        const auto& p=points[selected.index];
        auto& batch=batches[next.visible/kPointsPerBatch];
        const float radius=selected.radius;
        const float energy=selected.energy;
        const auto radiance=p.color*(p.intensity*energy);const uint16_t base=static_cast<uint16_t>(batch.vertices.size());
        for(int corner=0;corner<4;++corner) {
            const float x=(corner==1||corner==2)?1.f:-1.f,y=corner>=2?1.f:-1.f;
            const auto position=p.position+right*(radius*x)+up*(radius*y);
            batch.vertices.push_back({position.x,position.y,position.z,(x+1)*.5f,(y+1)*.5f,radiance.x,radiance.y,radiance.z,1});
        }
        for(const uint16_t offset:{0,1,2,0,2,3})batch.indices.push_back(base+offset);
        ++next.visible;
    }
    next.batches=static_cast<uint32_t>(needed);stats=next;
    if(error)error->clear();return true;
}
bool buildEmissivePointInstanceBatches(std::span<const EmissivePoint> points,
    const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
    EmissivePointSettings settings,std::vector<ParticleDrawData>& batches,
    EmissivePointStats& stats,std::string* error) {
    PointSelection selection;
    if(!selectVisiblePoints(points,view,projection,width,height,settings,selection,error))return false;
    auto next=selection.stats;
    const size_t needed=(selection.visible.size()+kPointsPerBatch-1)/kPointsPerBatch;
    batches.resize(needed);
    for(std::size_t i=0;i<needed;++i) {
        auto& batch=batches[i];
        const auto count=std::min(kPointsPerBatch,selection.visible.size()-i*kPointsPerBatch);
        batch.vertices.clear();batch.indices.clear();
        batch.instanceData.resize(count*16);
        batch.instanceStride=16*sizeof(float);batch.world3D=true;batch.gpuStream=nullptr;batch.submitGpu=nullptr;
    }
    for(const auto& selected:selection.visible) {
        const auto& point=points[selected.index];
        auto& batch=batches[next.visible/kPointsPerBatch];
        const auto radiance=point.color*(point.intensity*selected.energy);
        const float instance[16]={
            point.position.x,point.position.y,point.position.z,selected.radius,
            selection.right.x,selection.right.y,selection.right.z,0,
            selection.up.x,selection.up.y,selection.up.z,0,
            radiance.x,radiance.y,radiance.z,1
        };
        std::memcpy(batch.instanceData.data()+(next.visible%kPointsPerBatch)*16,instance,sizeof(instance));
        ++next.visible;
    }
    next.batches=static_cast<uint32_t>(needed);next.instanced=next.visible!=0;
    stats=next;if(error)error->clear();return true;
}
bool EmissivePointCloud::append(Renderer& renderer,RenderScene& scene,std::span<const EmissivePoint> points,
    const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
    EmissivePointSettings settings,std::string* error) {
    const auto* caps=bgfx::getCaps();
    const bool instancing=settings.preferInstancing&&caps&&(caps->supported&BGFX_CAPS_INSTANCING);
    bool gpu=false;auto fallback=EmissivePointGpuFallback::None;
    if(settings.preferGpuCulling&&!points.empty()){
        PointSelection validated;
        if(!validatePointInput(points,view,projection,width,height,settings,validated,error))return false;
        if(_renderer!=&renderer)_gpuState.reset();
        if(_gpuState&&!_gpuState->alive()){if(error)*error="Recreate point cloud after Renderer shutdown/reinitialize";return false;}
        if(points.size()>settings.maxPoints)fallback=EmissivePointGpuFallback::VisibleBudget;
        else if(!settings.preferInstancing)fallback=EmissivePointGpuFallback::InstancingDisabled;
        else if(!instancing)fallback=EmissivePointGpuFallback::Unavailable;
        else {
            if(!_gpuState||_gpuState.use_count()>1)_gpuState=renderer.createGpuPointState(error);
            if(_gpuState&&_gpuState->prepare(points,view,projection,width,height,settings,_stats,error)){
                _batches.resize(1);auto& batch=_batches.front();batch=ParticleDrawData{};batch.world3D=true;
                batch.gpuStream=_gpuState.get();batch.submitGpu=[](void* state,uint16_t id){static_cast<detail::GpuPointState*>(state)->submit(id);};gpu=true;
            }else{fallback=_gpuState?EmissivePointGpuFallback::Resources:EmissivePointGpuFallback::Unavailable;_gpuState.reset();}
        }
    }
    if(!gpu){
        const bool built=instancing
            ?buildEmissivePointInstanceBatches(points,view,projection,width,height,settings,_batches,_stats,error)
            :buildEmissivePointBatches(points,view,projection,width,height,settings,_batches,_stats,error);
        if(!built)return false;_stats.gpuFallback=fallback;
    }
    if(_batches.empty())return true;
    if(_renderer!=&renderer){_renderer=&renderer;_quad={};_texture={};_material={};_instancedMaterial={};}
    if(!_texture.isValid()) {
    uint8_t pixels[32*32*4];
    for(int y=0;y<32;++y)for(int x=0;x<32;++x) {
        const float dx=(x+.5f-16)/16,dy=(y+.5f-16)/16;const int i=(y*32+x)*4;
        pixels[i]=pixels[i+1]=pixels[i+2]=255;
        const float falloff=std::clamp(1-dx*dx-dy*dy,0.f,1.f);pixels[i+3]=uint8_t(255*falloff*falloff);
    }
    _texture=renderer.createTextureFromRgba8(32,32,pixels,"emissive_point_disc_v1");
    }
    auto& material=instancing?_instancedMaterial:_material;
    if(!material.isValid()) {
        material=renderer.createMaterialFromPhoskia(
            instancing?kEmissivePointInstancedPhoskiaSource:kParticlePhoskiaSource,
            instancing?"emissive_point_instanced_material_v1":"emissive_point_material_v1");
        renderer.setMaterialTexture(material,"albedoMap",_texture);
        renderer.setMaterialBlendMode(material,BlendMode::Additive);
        renderer.setMaterialSurfaceProperties(material,2,0,true);
        renderer.setMaterialModel(material,MaterialModel::Unlit);
    }
    if(!_quad.isValid())_quad=renderer.createUnitQuad();
    if(!_texture.isValid()||!material.isValid()||!_quad.isValid()){if(error)*error="Point cloud GPU assets unavailable";return false;}
    for(auto& batch:_batches) {
        DrawItem item;item.mesh=_quad;item.material=material;item.shadowFlags=ShadowFlags::None;
        item.particleBatch=&batch;item.allowFrustumCulling=false;scene.add(item);
    }
    return true;
}
} // namespace ayt::render
