#pragma once



#include "AYRenderer/ShadowConfig.h"



#include "AYMath/MathTypes.h"



#include <cmath>

#include <cstdio>



namespace ayt::render::detail

{



// CPU-side mirror of SimpleLitShadow fragment shadow UV / compare math.

// Uses the same column-major LVP bytes uploaded as u_lightViewProj.

struct ShadowProjectSample {

    float clipX = 0.0f;

    float clipY = 0.0f;

    float clipZ = 0.0f;

    float clipW = 0.0f;

    float ndcX  = 0.0f;

    float ndcY  = 0.0f;

    float rawDepth = 0.0f;

    float refDepth = 0.0f;

    float shadowU = 0.0f;

    float shadowV = 0.0f;

    bool  uvIn01  = false;

    float litIfOccluderIs1 = 0.0f;

    float litIfOccluderIs0 = 0.0f;

    float clearedIfOccluderIs1  = 0.0f;

};



inline ShadowProjectSample projectWorldThroughLvpColMajor(

    const float lvpColMajor[16],

    const ayt::math::FVector3& world,

    float shadowBias = ayt::render::kShadowBiasDefault)

{

    ShadowProjectSample out{};

    const float wx = world.x;

    const float wy = world.y;

    const float wz = world.z;



    out.clipX = lvpColMajor[0] * wx + lvpColMajor[4] * wy + lvpColMajor[8]  * wz + lvpColMajor[12];

    out.clipY = lvpColMajor[1] * wx + lvpColMajor[5] * wy + lvpColMajor[9]  * wz + lvpColMajor[13];

    out.clipZ = lvpColMajor[2] * wx + lvpColMajor[6] * wy + lvpColMajor[10] * wz + lvpColMajor[14];

    out.clipW = lvpColMajor[3] * wx + lvpColMajor[7] * wy + lvpColMajor[11] * wz + lvpColMajor[15];



    if (std::fabs(out.clipW) < 1.0e-6f) {

        return out;

    }



    const float invW = 1.0f / out.clipW;

    out.ndcX      = out.clipX * invW;

    out.ndcY      = out.clipY * invW;

    out.rawDepth  = out.clipZ * invW;

    out.refDepth  = ayt::render::ShadowDepthCodec::clipZOverWToNdc01(out.rawDepth);

    const float uy = out.ndcY * 0.5f + 0.5f;

    out.shadowU   = out.ndcX * 0.5f + 0.5f;

    out.shadowV   = 1.0f - uy;

    out.uvIn01    = out.shadowU >= 0.0f && out.shadowU <= 1.0f

                 && out.shadowV >= 0.0f && out.shadowV <= 1.0f;



    out.litIfOccluderIs1 =

        ayt::render::predictShadowLitFactor(out.refDepth, 1.0f, shadowBias);

    out.litIfOccluderIs0 =

        ayt::render::predictShadowLitFactor(out.refDepth, 0.0f, shadowBias);

    out.clearedIfOccluderIs1 =

        ayt::render::predictShadowLitFactor(out.refDepth, 1.0f, shadowBias);

    return out;

}



inline float predictLitFactor(float refDepth, float occluder, float shadowBias = ayt::render::kShadowBiasDefault)

{

    return ayt::render::predictShadowLitFactor(refDepth, occluder, shadowBias);

}



inline void logShadowProjectSample(const char* label, const ShadowProjectSample& s)

{

    std::fprintf(stderr,

                 "[ShadowDbg] %-22s clip=(% .4f,% .4f,% .4f,% .4f) "

                 "ndc=(% .4f,% .4f) rawZ=% .4f refN=% .4f uv=(% .4f,% .4f) in01=%d "

                 "lit(occl=1)=% .2f lit(occl=0)=% .2f cpuAssumedCleared=% .0f\n",

                 label,

                 s.clipX, s.clipY, s.clipZ, s.clipW,

                 s.ndcX, s.ndcY,

                 s.rawDepth,

                 s.refDepth,

                 s.shadowU, s.shadowV,

                 s.uvIn01 ? 1 : 0,

                 s.litIfOccluderIs1,

                 s.litIfOccluderIs0,

                 s.clearedIfOccluderIs1);

}



inline void logLvpMatrixColMajor(const char* tag, const float m[16])

{

    std::fprintf(stderr,

                 "[ShadowDbg] %s LVP col-major:\n"

                 "  [% .5f % .5f % .5f % .5f]\n"

                 "  [% .5f % .5f % .5f % .5f]\n"

                 "  [% .5f % .5f % .5f % .5f]\n"

                 "  [% .5f % .5f % .5f % .5f]\n",

                 tag,

                 m[0], m[1], m[2], m[3],

                 m[4], m[5], m[6], m[7],

                 m[8], m[9], m[10], m[11],

                 m[12], m[13], m[14], m[15]);

}



inline void logShadowPassCpuDiag(

    const ayt::math::FVector3& lightDirection,

    bool homogeneousDepth,

    const float lvpColMajor[16],

    float shadowBias = ayt::render::kShadowBiasDefault)

{

    static uint32_t s_frames = 0;

    if (s_frames >= 5) {

        return;

    }



    std::fprintf(stderr,

                 "[ShadowDbg] frame=%u lightDir=(%.3f,%.3f,%.3f) "

                 "homogeneousDepth=%d shadowBias=%.3f\n",

                 s_frames,

                 lightDirection.x, lightDirection.y, lightDirection.z,

                 homogeneousDepth ? 1 : 0,

                 shadowBias);

    logLvpMatrixColMajor("shadow-pass", lvpColMajor);



    const ayt::math::FVector3 cubeCenter(0.0f, 0.85f, 0.0f);

    const ayt::math::FVector3 groundUnderCube(0.0f, 0.0f, 0.0f);

    const ayt::math::FVector3 groundCorner(3.0f, 0.0f, 3.0f);

    const ayt::math::FVector3 cubeBottom(0.0f, 0.35f, 0.0f);



    logShadowProjectSample("cubeCenter",

                           projectWorldThroughLvpColMajor(lvpColMajor, cubeCenter, shadowBias));

    logShadowProjectSample("cubeBottom( est)",

                           projectWorldThroughLvpColMajor(lvpColMajor, cubeBottom, shadowBias));

    logShadowProjectSample("groundUnderCube",

                           projectWorldThroughLvpColMajor(lvpColMajor, groundUnderCube, shadowBias));

    logShadowProjectSample("groundCorner",

                           projectWorldThroughLvpColMajor(lvpColMajor, groundCorner, shadowBias));



    float uvMinU = 1.0f;

    float uvMaxU = 0.0f;

    float uvMinV = 1.0f;

    float uvMaxV = 0.0f;

    for (int ix = -1; ix <= 1; ix += 2) {

        for (int iy = -1; iy <= 1; iy += 2) {

            for (int iz = -1; iz <= 1; iz += 2) {

                const ayt::math::FVector3 corner(

                    cubeCenter.x + 0.5f * static_cast<float>(ix),

                    cubeCenter.y + 0.5f * static_cast<float>(iy),

                    cubeCenter.z + 0.5f * static_cast<float>(iz));

                const ShadowProjectSample s =

                    projectWorldThroughLvpColMajor(lvpColMajor, corner, shadowBias);

                uvMinU = std::fmin(uvMinU, s.shadowU);

                uvMaxU = std::fmax(uvMaxU, s.shadowU);

                uvMinV = std::fmin(uvMinV, s.shadowV);

                uvMaxV = std::fmax(uvMaxV, s.shadowV);

            }

        }

    }

    std::fprintf(stderr,

                 "[ShadowDbg] cubeAabbShadowUv u=[%.4f,%.4f] v=[%.4f,%.4f] "

                 "(ground needs overlap here for contact shadow)\n",

                 uvMinU, uvMaxU, uvMinV, uvMaxV);



    const ShadowProjectSample g =

        projectWorldThroughLvpColMajor(lvpColMajor, groundUnderCube, shadowBias);

    const ShadowProjectSample cubeBottomSample =

        projectWorldThroughLvpColMajor(lvpColMajor, cubeBottom, shadowBias);

    const bool groundInsideCubeAabb =

        g.shadowU >= uvMinU && g.shadowU <= uvMaxU

        && g.shadowV >= uvMinV && g.shadowV <= uvMaxV;

    std::fprintf(stderr,

                 "[ShadowDbg] groundUnderCube inside cubeUvAabb=%d "

                 "predictLit@ground if occluder=cubeBottom=% .2f "

                 "(0=shadow; cleared map occluder=1.0 forces lit via max())\n",

                 groundInsideCubeAabb ? 1 : 0,

                 predictLitFactor(g.refDepth, cubeBottomSample.refDepth, shadowBias));



    ++s_frames;

}



} // namespace ayt::render::detail

