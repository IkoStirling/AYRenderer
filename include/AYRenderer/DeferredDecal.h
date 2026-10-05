#pragma once
#include "AYRenderer/RenderTypes.h"
#include "AYMath/MathTypes.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
namespace ayt::render {
inline constexpr size_t kMaxDeferredDecals=16;
/// Advanced host projector: local cube [-1,1], local +Z faces receiver, UV=XY.
/// Renderer copies descriptors; texture handles borrow Renderer-owned resources.
struct DeferredDecal {
    math::Float4x4 localToWorld=math::Float4x4::identity();
    math::FVector3 color{1,1,1};
    float opacity=1;
    float normalStrength=0;
    float roughness=.5f;
    float metallic=0;
    float materialStrength=0;
    float minFacing=.1f;
    TextureHandle albedo{}; // optional sRGB color/alpha
    TextureHandle normal{}; // optional linear tangent-space normal
};
inline bool validDeferredDecalTransform(const math::Float4x4& m) noexcept {
    for(int i=0;i<16;++i)if(!std::isfinite(m.ptr()[i]))return false;
    for(int i=0;i<3;++i)if(std::abs(m.row[i].w)>100000)return false;
    if(std::abs(m.row[3].x)+std::abs(m.row[3].y)+std::abs(m.row[3].z)>1.e-6f||std::abs(m.row[3].w-1)>1.e-6f)return false;
    // Orthogonal positive-scale affine basis only; shears/mirrors would need a
    // distinct tangent handedness contract and inverse-transpose normals.
    math::FVector3 axes[3]={{m.row[0].x,m.row[1].x,m.row[2].x},{m.row[0].y,m.row[1].y,m.row[2].y},{m.row[0].z,m.row[1].z,m.row[2].z}};
    for(auto& axis:axes){float l=axis.length();if(!std::isfinite(l)||l<.001f||l>10000)return false;axis=axis/l;}
    if(std::abs(axes[0].dot(axes[1]))>.001f||std::abs(axes[1].dot(axes[2]))>.001f||std::abs(axes[2].dot(axes[0]))>.001f)return false;
    return m.determinant()>0;
}
inline DeferredDecal sanitizeDeferredDecal(DeferredDecal d) noexcept {
    auto finite=[](float x,float fallback,float lo,float hi){return std::isfinite(x)?std::clamp(x,lo,hi):fallback;};
    d.opacity=finite(d.opacity,0,0,1);d.normalStrength=finite(d.normalStrength,0,0,1);
    d.roughness=finite(d.roughness,.5f,.045f,1);d.metallic=finite(d.metallic,0,0,1);d.materialStrength=finite(d.materialStrength,0,0,1);d.minFacing=finite(d.minFacing,.1f,0,.99f);
    for(int i=0;i<3;++i)d.color[i]=finite(d.color[i],1,0,1);
    if(!validDeferredDecalTransform(d.localToWorld))d.opacity=0;
    return d;
}
}
