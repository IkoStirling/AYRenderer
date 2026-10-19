#include <AYRenderer/VolumetricDensity.h>
#include <AYRenderer/FixtureEmitters.h>
#include <AYRenderer.h>
#include <algorithm>
#include <cmath>
#include <numbers>
namespace ayt::render {
namespace {
float smooth(float a,float b,float v){const float t=std::clamp((v-a)/(b-a),0.f,1.f);return t*t*(3-2*t);}
float transmission(const math::FVector3& origin,const math::FVector3& target,VolumetricSettings fog,const VolumetricDensityField* field){
    fog=sanitizeVolumetricSettings(fog);if(field)fog.local=true;if(!fog.enabled||fog.density<=0)return 1;
    const auto delta=target-origin;const float distance=delta.length();if(distance<1e-6f)return 1;
    const auto direction=delta/distance;float begin=0,end=std::min(distance,fog.maxDistance);
    if(fog.local)for(int i=0;i<3;++i){
        if(std::abs(direction[i])<1e-7f){if(std::abs(origin[i]-fog.center[i])>fog.halfExtent[i])return 1;}
        else{
            const float a=(fog.center[i]-fog.halfExtent[i]-origin[i])/direction[i];
            const float b=(fog.center[i]+fog.halfExtent[i]-origin[i])/direction[i];
            begin=std::max(begin,std::min(a,b));end=std::min(end,std::max(a,b));
        }
    }
    if(end<=begin)return 1;
    if(!field&&!fog.local&&fog.noiseStrength<=.001f)return std::exp(-fog.density*(end-begin));
    const float ds=(end-begin)/16;float opticalDepth=0;
    for(int i=0;i<16;++i){
        const auto p=origin+direction*(begin+(i+.5f)*ds);float weight=1;
        if(fog.local){float edge=0;for(int j=0;j<3;++j)edge=std::max(edge,std::abs(p[j]-fog.center[j])/fog.halfExtent[j]);weight=1-smooth(.75f,1,edge);}
        if(fog.noiseStrength>.001f){
            const auto q=p*fog.noiseScale+math::FVector3(fog.noiseTime*.17f,fog.noiseTime*.11f,fog.noiseTime*.14f);
            weight*=std::clamp(1+fog.noiseStrength*(.5f*std::sin(q.x+q.z*.73f)+.35f*std::sin(q.y*1.7f-q.z*1.1f)+.15f*std::sin(q.x*1.3f+q.y*.6f)),.15f,1.85f);
        }
        if(field)weight*=field->sample({(p.x-fog.center.x)/fog.halfExtent.x,(p.y-fog.center.y)/fog.halfExtent.y,(p.z-fog.center.z)/fog.halfExtent.z});
        opticalDepth+=fog.density*ds*weight;
    }
    return std::exp(-opticalDepth);
}
}
bool buildFixtureEmitterBatch(std::span<const PerformanceLight> lights,
    const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
    FixtureEmitterSettings settings,const VolumetricSettings& medium,
    ParticleDrawData& output,FixtureEmitterStats& stats,std::string* error,const VolumetricDensityField* density){
    const auto fail=[&](const char* message){if(error)*error=message;return false;};
    const auto finite=[](float v,float lo,float hi){return std::isfinite(v)&&v>=lo&&v<=hi;};
    if(!width||!height||!finite(settings.radianceScale,0,100)||!finite(settings.maximumRadiance,1,60000)
        ||!finite(settings.minimumPixelRadius,0,4)||!finite(settings.offAxisGlow,0,1))return fail("Invalid fixture emitter settings/viewport");
    for(int i=0;i<16;++i)if(!std::isfinite(view.ptr()[i])||!std::isfinite(projection.ptr()[i]))return fail("Invalid fixture emitter camera");
    const auto inverse=view.inverse();
    for(int i=0;i<16;++i)if(!std::isfinite(inverse.ptr()[i]))return fail("Singular fixture emitter view");
    const math::FVector3 forward{inverse(0,2),inverse(1,2),inverse(2,2)},eye{inverse(0,3),inverse(1,3),inverse(2,3)};
    const float pixelScale=std::abs(projection(1,1))*height*.5f;
    if(forward.length()<.00001f||!std::isfinite(pixelScale)||pixelScale<.00001f)return fail("Degenerate fixture emitter camera");
    PerformanceLighting packet;if(!packet.assign(lights,{},error))return false;
    ParticleDrawData batch;batch.world3D=true;batch.vertices.reserve(lights.size()*4);batch.indices.reserve(lights.size()*6);
    FixtureEmitterStats next;next.source=uint32_t(lights.size());const auto vp=projection*view;
    const bool orthographic=std::abs(projection(3,3))>.5f;
    for(const auto& entry:packet.lights){
        const auto& l=entry.light;
        if(l.type!=LightType::Spot||entry.apertureRadius<=0||l.intensity<=0||settings.radianceScale<=0){++next.culled;continue;}
        auto toEye=eye-l.position;float distance=toEye.length();
        if(orthographic){toEye=forward.normalize()*-1;distance=std::max(0.f,(view*math::FVector4(l.position.x,l.position.y,l.position.z,1)).z);}
        else {if(distance<.00001f){++next.culled;continue;}toEye=toEye/distance;}
        const float facing=l.spotDirection.dot(toEye);
        if(facing<=0){++next.backFacing;continue;}
        const auto clip=vp*math::FVector4(l.position.x,l.position.y,l.position.z,1);
        if(!std::isfinite(clip.x)||!std::isfinite(clip.y)||!std::isfinite(clip.w))return fail("Non-finite fixture projection");
        if(clip.w<=.00001f){++next.culled;continue;}
        const float physicalRadius=entry.apertureRadius*entry.iris;
        if(physicalRadius<=0){++next.culled;continue;}
        const float radius=std::max(physicalRadius,settings.minimumPixelRadius*clip.w/pixelScale);
        const float marginX=radius*std::abs(projection(0,0)),marginY=radius*std::abs(projection(1,1));
        if(clip.x< -clip.w-marginX||clip.x>clip.w+marginX||clip.y< -clip.w-marginY||clip.y>clip.w+marginY){++next.culled;continue;}
        const float angular=performanceLightAngular(entry,toEye,settings.offAxisGlow);
        const double footprintRatio=double(physicalRadius)/radius;
        const float energy=float(footprintRatio*footprintRatio);
        const float throughFog=transmission(orthographic?l.position+toEye*distance:eye,l.position,medium,density);
        // Double keeps small apertures finite before the explicit HDR bound.
        const double scale=double(performanceLightIntensity(entry))*settings.radianceScale
            *angular/(entry.intensityUnit==LightIntensityUnit::Legacy?1.0:std::max(double(facing),1e-6))/(std::numbers::pi*double(entry.apertureRadius)*entry.apertureRadius);
        math::FVector3 color;bool clamped=false;
        for(int i=0;i<3;++i){const double value=scale*l.color[i];clamped|=value>settings.maximumRadiance;color[i]=float(std::min(value,double(settings.maximumRadiance))*throughFog*energy);}
        if(color.x<=0&&color.y<=0&&color.z<=0){++next.culled;continue;}
        next.clamped+=clamped?1:0;
        const auto d=l.spotDirection,axis=std::abs(d.y)>.99f?math::FVector3(0,0,1):math::FVector3(0,1,0);
        const auto right=axis.cross(d).normalize(),up=d.cross(right).normalize();const uint16_t base=uint16_t(batch.vertices.size());
        for(int corner=0;corner<4;++corner){
            const float x=(corner==1||corner==2)?1.f:-1.f,y=corner>=2?1.f:-1.f;
            const auto p=l.position+right*(radius*x)+up*(radius*y);
            batch.vertices.push_back({p.x,p.y,p.z,(x+1)*.5f,(y+1)*.5f,color.x,color.y,color.z,1});
        }
        for(const uint16_t offset:{0,1,2,0,2,3})batch.indices.push_back(base+offset);++next.prepared;
    }
    output=std::move(batch);stats=next;if(error)error->clear();return true;
}
bool FixtureEmitters::append(Renderer& renderer,RenderScene& scene,std::span<const PerformanceLight> lights,
    const math::Float4x4& view,const math::Float4x4& projection,uint16_t width,uint16_t height,
    FixtureEmitterSettings settings,std::string* error){
    if(!buildFixtureEmitterBatch(lights,view,projection,width,height,settings,renderer.volumetricSettings(),_batch,_stats,error,renderer.volumetricDensityField().profile.get()))return false;
    if(_batch.vertices.empty())return true;
    if(_renderer!=&renderer){_renderer=&renderer;_quad={};_texture={};_material={};}
    if(!_texture.isValid()){
        uint8_t pixels[64*64*4];
        for(int y=0;y<64;++y)for(int x=0;x<64;++x){
            const float dx=(x+.5f-32)/32,dy=(y+.5f-32)/32;const int i=(y*64+x)*4;
            pixels[i]=pixels[i+1]=pixels[i+2]=255;pixels[i+3]=uint8_t(255*(1-smooth(.9f,1,std::sqrt(dx*dx+dy*dy))));
        }
        _texture=renderer.createTextureFromRgba8(64,64,pixels,"fixture_aperture_disc_v1");
    }
    if(!_material.isValid()){
        _material=renderer.createMaterialFromPhoskia(kParticlePhoskiaSource,"fixture_aperture_hdr_v1");
        renderer.setMaterialTexture(_material,"albedoMap",_texture);renderer.setMaterialBlendMode(_material,BlendMode::Additive);
        renderer.setMaterialSurfaceProperties(_material,2,0,true);renderer.setMaterialModel(_material,MaterialModel::Unlit);
    }
    if(!_quad.isValid())_quad=renderer.createUnitQuad();
    if(!_texture.isValid()||!_material.isValid()||!_quad.isValid()){if(error)*error="Fixture emitter GPU assets unavailable";return false;}
    DrawItem item;item.mesh=_quad;item.material=_material;item.shadowFlags=ShadowFlags::None;item.particleBatch=&_batch;item.allowFrustumCulling=false;
    scene.add(item);if(error)error->clear();return true;
}
} // namespace ayt::render
