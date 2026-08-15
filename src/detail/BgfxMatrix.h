#pragma once

// AYMath Float4x4 is row-major (translation in row[i].w).
// bgfx expects column-major (translation in m[12..14]).
// All engine-side matrices stay true AYMath; convert only at the
// GPU upload boundary via these helpers.

#include "AYMath/MathTypes.h"

namespace ayt::render::detail
{

inline void toBgfxColumnMajor(const ayt::math::Float4x4& src, float dst[16])
{
    const float* s = src.ptr();
    dst[0]  = s[0];  dst[1]  = s[4];  dst[2]  = s[8];  dst[3]  = s[12];
    dst[4]  = s[1];  dst[5]  = s[5];  dst[6]  = s[9];  dst[7]  = s[13];
    dst[8]  = s[2];  dst[9]  = s[6];  dst[10] = s[10]; dst[11] = s[14];
    dst[12] = s[3];  dst[13] = s[7];  dst[14] = s[11]; dst[15] = s[15];
}

inline ayt::math::Float4x4 fromBgfxColumnMajor(const float src[16])
{
    ayt::math::Float4x4 out = ayt::math::Float4x4::identity();
    float* d = out.ptr();
    // Inverse of toBgfxColumnMajor (transpose).
    d[0]  = src[0];  d[1]  = src[4];  d[2]  = src[8];  d[3]  = src[12];
    d[4]  = src[1];  d[5]  = src[5];  d[6]  = src[9];  d[7]  = src[13];
    d[8]  = src[2];  d[9]  = src[6];  d[10] = src[10]; d[11] = src[14];
    d[12] = src[3];  d[13] = src[7];  d[14] = src[11]; d[15] = src[15];
    return out;
}

} // namespace ayt::render::detail
