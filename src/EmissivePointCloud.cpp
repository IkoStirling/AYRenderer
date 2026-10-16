#include <AYRenderer/EmissivePointCloud.h>
#include <AYRenderer.h>
#include <bgfx/bgfx.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace ayt::render {
namespace {
struct VisiblePoint { std::size_t index; float radius,energy; std::uint32_t rank; };
struct PointSelection {
    std::vector<VisiblePoint> visible;
    math::FVector3 right,up;
    EmissivePointStats stats;
};
constexpr std::size_t kPointsPerBatch=16383;

bool selectVisiblePoints(std::span<const EmissivePoint> points,const math::Float4x4& view,
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
    const auto vp=projection*view;const float scale=std::abs(projection(1,1))*height*.5f;
    if(scale<.00001f)return fail("Degenerate point cloud projection");
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
        b.instanceData.clear();b.instanceStride=0;b.world3D=true;
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
        batch.vertices.clear();batch.indices.clear();batch.instanceData.clear();
        batch.instanceData.reserve(count*16);
        batch.instanceStride=16*sizeof(float);batch.world3D=true;
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
        batch.instanceData.insert(batch.instanceData.end(),std::begin(instance),std::end(instance));
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
    const bool built=instancing
        ?buildEmissivePointInstanceBatches(points,view,projection,width,height,settings,_batches,_stats,error)
        :buildEmissivePointBatches(points,view,projection,width,height,settings,_batches,_stats,error);
    if(!built)return false;
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
