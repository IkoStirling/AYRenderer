#pragma once
#include "AYMath/MathTypes.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ayt::render {
/// Advanced host controls and optional axis-aligned local medium. Legacy mode
/// selects one SceneLight; an active performance packet uses its separate budget.
struct VolumetricSettings {
    bool enabled = false;
    float density = 0.025f; // extinction, inverse world units
    float scattering = 0.8f; // single-scattering albedo, [0,1]
    float anisotropy = 0.2f;
    float maxDistance = 100.0f;
    uint8_t steps = 32;
    /// Per-axis scattering resolution divisor: 1 legacy, 2 half, 4 quarter.
    /// Full-resolution extinction and depth-aware reconstruction preserve scene detail.
    uint8_t downsample = 1;
    uint8_t lightIndex = 0; // Legacy single-light selection; performance packets ignore it.
    uint8_t debugView = 0; // 0 composite, 1 transmittance, 2 radiance
    bool local = false;
    math::FVector3 center{0,0,0};
    math::FVector3 halfExtent{10,10,10};
    math::FVector3 tint{1,1,1};
    /// Optional animated density variation. Zero keeps the original uniform
    /// medium; positive strength modulates extinction and in-scattering.
    float noiseStrength = 0;
    float noiseScale = 0.2f; // cycles per world unit, [0.01,2]
    float noiseTime = 0; // seconds, supplied by the host's show clock
    /// Optional bounded view-frustum coefficient grid. Default false keeps ray marching.
    /// Grid XY resolution is capped by texture limits/cell budget; downsample is ray-only.
    bool froxel = false;
    uint8_t froxelTileSize = 8; // requested pixels/cell in XY: 4,8,16
    uint8_t froxelSlices = 32; // logarithmic observer-distance slices:16,32,64
    uint32_t froxelMaxCells = 1048576; // [65536,4194304], allocated cells never exceed this
};
inline VolumetricSettings sanitizeVolumetricSettings(VolumetricSettings s) noexcept {
    auto finite=[](float x,float fallback,float lo,float hi){return std::isfinite(x)?std::clamp(x,lo,hi):fallback;};
    s.density=finite(s.density,.025f,0,1); s.scattering=finite(s.scattering,.8f,0,1);
    s.anisotropy=finite(s.anisotropy,.2f,-.9f,.9f); s.maxDistance=finite(s.maxDistance,100,.1f,500);
    s.downsample=s.downsample>=4?4:s.downsample>=2?2:1;
    s.steps=std::clamp<uint8_t>(s.steps,8,64); s.lightIndex=std::min<uint8_t>(s.lightIndex,7); s.debugView=std::min<uint8_t>(s.debugView,2);
    for(int i=0;i<3;++i) {s.center[i]=finite(s.center[i],0,-100000,100000);s.halfExtent[i]=finite(s.halfExtent[i],10,.01f,10000);s.tint[i]=finite(s.tint[i],1,0,1);}
    s.froxelTileSize=s.froxelTileSize>=16?16:s.froxelTileSize>=8?8:4;
    s.froxelSlices=s.froxelSlices>=64?64:s.froxelSlices>=32?32:16;
    s.froxelMaxCells=std::clamp<uint32_t>(s.froxelMaxCells,65536,4194304);
    s.noiseStrength=finite(s.noiseStrength,0,0,1);
    s.noiseScale=finite(s.noiseScale,.2f,.01f,2);
    s.noiseTime=finite(s.noiseTime,0,-100000,100000);
    return s;
}
}
