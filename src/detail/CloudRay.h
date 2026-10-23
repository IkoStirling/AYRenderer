#pragma once
#include "AYMath/MathTypes.h"
#include <algorithm>
#include <cmath>
namespace ayt::render::detail {
// General projective inverse with double elimination. Reject singular/nonfinite camera transforms.
inline bool cloudInverse(const math::Float4x4& vp,math::Float4x4& output){
 double rows[4][8]{};for(int r=0;r<4;++r)for(int c=0;c<4;++c){const auto v=vp.ptr()[r*4+c];if(!std::isfinite(v))return false;rows[r][c]=v;rows[r][c+4]=r==c?1:0;}
 for(int c=0;c<4;++c){int pivot=c;for(int r=c+1;r<4;++r)if(std::abs(rows[r][c])>std::abs(rows[pivot][c]))pivot=r;if(std::abs(rows[pivot][c])<1e-12)return false;
  for(int j=0;j<8;++j)std::swap(rows[c][j],rows[pivot][j]);const auto divisor=rows[c][c];for(double& v:rows[c])v/=divisor;
  for(int r=0;r<4;++r)if(r!=c){const auto scale=rows[r][c];for(int j=0;j<8;++j)rows[r][j]-=scale*rows[c][j];}}
 math::Float4x4 candidate;for(int r=0;r<4;++r)for(int c=0;c<4;++c){auto value=rows[r][c+4];if(!std::isfinite(value)||std::abs(value)>1e12)return false;candidate.ptr()[r*4+c]=float(value);}
 for(int r=0;r<4;++r)for(int c=0;c<4;++c){double residual=0;for(int k=0;k<4;++k)residual+=double(vp.ptr()[r*4+k])*candidate.ptr()[k*4+c];if(std::abs(residual-(r==c?1.:0.))>.001)return false;}
 output=candidate;return true;
}
}
