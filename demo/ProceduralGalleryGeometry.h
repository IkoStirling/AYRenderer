#pragma once
#include "AYRenderer.h"
#include <array>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace ayt::render::demo {
struct GalleryVertex {float x,y,z,nx,ny,nz,tx,ty,tz,tw,u,v;};
struct GalleryGeometry {std::vector<GalleryVertex> vertices;std::vector<uint16_t> indices;};
inline MeshHandle uploadGalleryGeometry(Renderer& renderer,const GalleryGeometry& mesh) {
    if(mesh.vertices.empty() || mesh.vertices.size()>65535 || mesh.indices.empty())return {};
    for(auto index:mesh.indices)if(index>=mesh.vertices.size())return {};
    VertexLayoutDesc layout;
    layout.add({VertexAttribute::Position,3,VertexComponentType::Float,false});
    layout.add({VertexAttribute::Normal,3,VertexComponentType::Float,false});
    layout.add({VertexAttribute::Tangent,4,VertexComponentType::Float,false});
    layout.add({VertexAttribute::TexCoord0,2,VertexComponentType::Float,false});
    return renderer.createMesh(mesh.vertices.data(),uint32_t(mesh.vertices.size()),layout,mesh.indices.data(),uint32_t(mesh.indices.size()));
}
// Face-separated positions/normals/UVs: the convenience textured cube has
// positions and UVs only, so a lit gallery supplies an explicit normal stream.
inline GalleryGeometry galleryCube()
{
    const float points[6][4][3]={
        {{-.5f,-.5f,-.5f},{.5f,-.5f,-.5f},{.5f,.5f,-.5f},{-.5f,.5f,-.5f}},
        {{-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}},
        {{.5f,-.5f,-.5f},{.5f,-.5f,.5f},{.5f,.5f,.5f},{.5f,.5f,-.5f}},
        {{-.5f,-.5f,.5f},{-.5f,-.5f,-.5f},{-.5f,.5f,-.5f},{-.5f,.5f,.5f}},
        {{-.5f,.5f,-.5f},{.5f,.5f,-.5f},{.5f,.5f,.5f},{-.5f,.5f,.5f}},
        {{-.5f,-.5f,.5f},{.5f,-.5f,.5f},{.5f,-.5f,-.5f},{-.5f,-.5f,-.5f}}};
    const float normals[6][3]={{0,0,-1},{0,0,1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    using Vertex=GalleryVertex;
    std::array<Vertex,24> vertices{};std::array<uint16_t,36> indices{};
    const uint16_t flipped[6]={0,2,1,0,3,2},straight[6]={0,1,2,0,2,3};
    for(int face=0;face<6;++face) {
        for(int corner=0;corner<4;++corner) {
            auto* p=points[face][corner];auto* n=normals[face];
            vertices[face*4+corner]={p[0],p[1],p[2],n[0],n[1],n[2],
                points[face][1][0]-points[face][0][0],
                points[face][1][1]-points[face][0][1],
                points[face][1][2]-points[face][0][2],face==1 ? -1.0f : 1.0f,
                corner==1 || corner==2 ? 1.0f : 0.0f,corner<2 ? 1.0f : 0.0f};
        }
        for(int j=0;j<6;++j)indices[face*6+j]=uint16_t(face*4+(face==1?straight[j]:flipped[j]));
    }
    GalleryGeometry mesh;
    mesh.vertices.assign(vertices.begin(),vertices.end());mesh.indices.assign(indices.begin(),indices.end());return mesh;
}
inline MeshHandle createGalleryCube(Renderer& renderer){return uploadGalleryGeometry(renderer,galleryCube());}
inline GalleryGeometry gallerySphere() {
    constexpr int rows=16,columns=32;constexpr float pi=3.14159265358979323846f;
    GalleryGeometry mesh;
    for(int y=0;y<=rows;++y)for(int x=0;x<=columns;++x) {
        const float theta=pi*y/rows,phi=2*pi*x/columns;
        const float nx=std::sin(theta)*std::cos(phi),ny=std::cos(theta),nz=std::sin(theta)*std::sin(phi);
        mesh.vertices.push_back({nx*.5f,ny*.5f,nz*.5f,nx,ny,nz,-std::sin(phi),0,std::cos(phi),1,float(x)/columns,float(y)/rows});
    }
    for(int y=0;y<rows;++y)for(int x=0;x<columns;++x) {
        const auto a=uint16_t(y*(columns+1)+x),b=uint16_t(a+columns+1);
        if(y+1<rows)mesh.indices.insert(mesh.indices.end(),{a,uint16_t(b+1),b});
        if(y>0)mesh.indices.insert(mesh.indices.end(),{a,uint16_t(a+1),uint16_t(b+1)});
    }
    return mesh;
}
inline GalleryGeometry galleryCylinder() {
    constexpr int columns=32;constexpr float tau=6.2831853071795864769f;GalleryGeometry mesh;
    for(int row=0;row<2;++row)for(int i=0;i<=columns;++i) {
        const float phi=tau*i/columns,c=std::cos(phi),s=std::sin(phi);
        mesh.vertices.push_back({c*.5f,row-.5f,s*.5f,c,0,s,-s,0,c,-1,float(i)/columns,float(row)});
    }
    for(int i=0;i<columns;++i) {
        const auto a=uint16_t(i),b=uint16_t(i+columns+1);
        mesh.indices.insert(mesh.indices.end(),{a,b,uint16_t(b+1),a,uint16_t(b+1),uint16_t(a+1)});
    }
    for(int top=0;top<2;++top) {
        const auto center=uint16_t(mesh.vertices.size());const float y=top-.5f,normal=top?1.f:-1.f;
        mesh.vertices.push_back({0,y,0,0,normal,0,1,0,0,-normal,.5f,.5f});
        for(int i=0;i<=columns;++i) {
            const float phi=tau*i/columns,c=std::cos(phi),s=std::sin(phi);
            mesh.vertices.push_back({c*.5f,y,s*.5f,0,normal,0,1,0,0,-normal,.5f+c*.5f,.5f+s*.5f});
        }
        for(int i=0;i<columns;++i) {
            const auto a=uint16_t(center+1+i),b=uint16_t(a+1);
            mesh.indices.insert(mesh.indices.end(),{center,top?b:a,top?a:b});
        }
    }
    return mesh;
}
inline GalleryGeometry galleryTorus() {
    constexpr int rows=12,columns=32;constexpr float tau=6.2831853071795864769f;GalleryGeometry mesh;
    for(int row=0;row<=rows;++row)for(int i=0;i<=columns;++i) {
        const float u=tau*i/columns,v=tau*row/rows,c=std::cos(u),s=std::sin(u),cv=std::cos(v),sv=std::sin(v);
        mesh.vertices.push_back({(.5f+.04f*cv)*c,.04f*sv,(.5f+.04f*cv)*s,cv*c,sv,cv*s,-s,0,c,-1,float(i)/columns,float(row)/rows});
    }
    for(int row=0;row<rows;++row)for(int i=0;i<columns;++i) {
        const auto a=uint16_t(row*(columns+1)+i),b=uint16_t(a+columns+1);
        mesh.indices.insert(mesh.indices.end(),{a,b,uint16_t(b+1),a,uint16_t(b+1),uint16_t(a+1)});
    }
    return mesh;
}
inline GalleryGeometry galleryRoof() {
    GalleryGeometry mesh;
    const auto face=[&](std::initializer_list<std::array<float,3>> points) {
        const std::vector<std::array<float,3>> p(points);const auto base=uint16_t(mesh.vertices.size());
        const float ax=p[1][0]-p[0][0],ay=p[1][1]-p[0][1],az=p[1][2]-p[0][2];
        const float bx=p[2][0]-p[0][0],by=p[2][1]-p[0][1],bz=p[2][2]-p[0][2];
        const float nx=ay*bz-az*by,ny=az*bx-ax*bz,nz=ax*by-ay*bx,n=std::sqrt(nx*nx+ny*ny+nz*nz),t=std::sqrt(ax*ax+ay*ay+az*az);
        for(size_t i=0;i<p.size();++i)mesh.vertices.push_back({p[i][0],p[i][1],p[i][2],nx/n,ny/n,nz/n,ax/t,ay/t,az/t,1,i==1||i==2?1.f:0.f,i<2?1.f:0.f});
        mesh.indices.insert(mesh.indices.end(),{base,uint16_t(base+1),uint16_t(base+2)});
        if(p.size()==4)mesh.indices.insert(mesh.indices.end(),{base,uint16_t(base+2),uint16_t(base+3)});
    };
    face({{-.5f,0,-.5f},{0,.5f,-.5f},{.5f,0,-.5f}});
    face({{.5f,0,.5f},{0,.5f,.5f},{-.5f,0,.5f}});
    face({{-.5f,0,.5f},{0,.5f,.5f},{0,.5f,-.5f},{-.5f,0,-.5f}});
    face({{0,.5f,.5f},{.5f,0,.5f},{.5f,0,-.5f},{0,.5f,-.5f}});
    face({{-.5f,0,-.5f},{.5f,0,-.5f},{.5f,0,.5f},{-.5f,0,.5f}});
    return mesh;
}
}
