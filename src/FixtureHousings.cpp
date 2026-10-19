#include <AYRenderer/FixtureHousings.h>
#include <AYRenderer.h>
#include <AYRenderer/PbrShaderSources.h>
#include <cmath>
#include <numbers>
namespace ayt::render {
namespace {
math::Float4x4 transform(math::FVector3 position,math::FVector3 right,math::FVector3 up,math::FVector3 axis){
    auto m=math::Float4x4::identity();
    m.row[0]={right.x,up.x,axis.x,position.x};m.row[1]={right.y,up.y,axis.y,position.y};m.row[2]={right.z,up.z,axis.z,position.z};return m;
}
struct Vertex {float x,y,z,nx,ny,nz,u,v;};
MeshHandle tube(Renderer& renderer,bool hood){
    std::vector<Vertex> vertices;std::vector<uint16_t> indices;
    const auto quad=[&](math::FVector3 a,math::FVector3 b,math::FVector3 c,math::FVector3 d,math::FVector3 n){
        const uint16_t base=uint16_t(vertices.size());for(const auto p:{a,b,c,d})vertices.push_back({p.x,p.y,p.z,n.x,n.y,n.z,0,0});
        if((b-a).cross(c-a).dot(n)>0)for(uint16_t i:{0,1,2,0,2,3})indices.push_back(base+i);
        else for(uint16_t i:{0,2,1,0,3,2})indices.push_back(base+i);
    };
    const auto disk=[&](math::FVector3 a,math::FVector3 b,float z,float nz){const uint16_t base=uint16_t(vertices.size());
        vertices.push_back({0,0,z,0,0,nz,0,0});vertices.push_back({a.x,a.y,z,0,0,nz,0,0});vertices.push_back({b.x,b.y,z,0,0,nz,0,0});
        indices.push_back(base);indices.push_back(base+(nz>0?1:2));indices.push_back(base+(nz>0?2:1));
    };
    constexpr int segments=16;const float lo=hood?0.f:-1.f,hi=hood?1.f:0.f;
    for(int i=0;i<segments;++i){const float a=float(2*std::numbers::pi*i/segments),b=float(2*std::numbers::pi*(i+1)/segments),mid=(a+b)*.5f;
        const math::FVector3 x{std::cos(a),std::sin(a),0},y{std::cos(b),std::sin(b),0},n{std::cos(mid),std::sin(mid),0};
        const auto point=[](math::FVector3 p,float r,float z){return math::FVector3(p.x*r,p.y*r,z);};
        quad(point(x,1.35f,lo),point(y,1.35f,lo),point(y,1.35f,hi),point(x,1.35f,hi),n);
        quad(point(x,1.02f,lo),point(y,1.02f,lo),point(y,1.02f,hi),point(x,1.02f,hi),n*-1);
        quad(point(x,1.02f,hi),point(y,1.02f,hi),point(y,1.35f,hi),point(x,1.35f,hi),{0,0,1});
        if(!hood){disk(x*1.35f,y*1.35f,-1,-1);disk(x*1.02f,y*1.02f,-.98f,1);}
    }
    return renderer.createMesh(vertices.data(),uint32_t(vertices.size()),VertexLayoutDesc::position3Normal3TexCoord2(),indices.data(),uint32_t(indices.size()));
}
}
bool buildFixtureHousingDraws(std::span<const PerformanceLight> lights,FixtureHousingSettings s,
    std::vector<FixtureHousingDraw>& output,FixtureHousingStats& stats,std::string* error){
    const auto valid=[](float x,float low,float high){return std::isfinite(x)&&x>=low&&x<=high;};
    if(!valid(s.bodyLength,.5f,10)||!valid(s.hoodLength,0,4)||!valid(s.leafAngle,-.5f,.5f)){if(error)*error="Invalid fixture housing dimensions";return false;}
    PerformanceLighting packet;if(!packet.assign(lights,{},error))return false;
    FixtureHousingStats next;next.source=uint32_t(lights.size());std::vector<FixtureHousingDraw> draws;draws.reserve(lights.size()*6);
    for(const auto& light:packet.lights){const auto& l=light.light;const auto radius=light.apertureRadius;if(l.type!=LightType::Spot||radius<=0)continue;
        const auto axis=l.spotDirection,reference=std::abs(axis.y)>.99f?math::FVector3(0,0,1):math::FVector3(0,1,0),right=reference.cross(axis).normalize(),up=axis.cross(right).normalize();
        draws.push_back({light.id,FixtureHousingPart::Body,transform(l.position,right*radius,up*radius,axis*(radius*s.bodyLength))});
        if(s.hoodLength>0)draws.push_back({light.id,FixtureHousingPart::Hood,transform(l.position,right*radius,up*radius,axis*(radius*s.hoodLength))});
        if(s.shadeLeaves)for(int i=0;i<4;++i){const float a=float(std::numbers::pi*.5*i);const auto radial=right*std::cos(a)+up*std::sin(a),tangent=axis.cross(radial).normalize();
            const auto along=axis*std::cos(s.leafAngle)-radial*std::sin(s.leafAngle),normal=along.cross(tangent).normalize();
            const auto center=l.position+axis*(radius*s.hoodLength)+radial*(1.35f*radius)+along*(.6f*radius);
            draws.push_back({light.id,FixtureHousingPart::Leaf,transform(center,tangent*(.72f*radius),normal*(.02f*radius),along*(.6f*radius))});
        }
        ++next.fixtures;
    }
    next.parts=uint32_t(draws.size());output=std::move(draws);stats=next;if(error)error->clear();return true;
}
bool FixtureHousings::append(Renderer& renderer,RenderScene& scene,std::span<const PerformanceLight> lights,
    FixtureHousingSettings settings,std::string* error){
    if(!buildFixtureHousingDraws(lights,settings,_draws,_stats,error))return false;
    if(_draws.empty())return true;
    if(_renderer!=&renderer){_renderer=&renderer;_body={};_hood={};_leaf={};_material={};}
    if(!_body.isValid())_body=tube(renderer,false);if(!_hood.isValid())_hood=tube(renderer,true);if(!_leaf.isValid())_leaf=renderer.createTexturedUnitCube();
    if(!_material.isValid()){
        _material=renderer.createMaterialFromPhoskia(kPbrPhoskiaSource,"fixture_housing_matte_v1");
        renderer.setMaterialColor(_material,"baseColor",.012f,.014f,.018f,1);renderer.setMaterialFloat(_material,"roughness",.6f);renderer.setMaterialFloat(_material,"metallic",.4f);
    }
    if(!_body.isValid()||!_hood.isValid()||!_leaf.isValid()||!_material.isValid()){if(error)*error="Fixture housing GPU assets unavailable";return false;}
    for(const auto& draw:_draws)scene.add(draw.part==FixtureHousingPart::Body?_body:draw.part==FixtureHousingPart::Hood?_hood:_leaf,_material,draw.world);
    if(error)error->clear();return true;
}
} // namespace ayt::render
