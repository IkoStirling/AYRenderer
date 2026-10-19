#pragma once
namespace ayt::render {
// Deterministic integer-domain arithmetic represented exactly by shader floats
// before bounded division. This is seeded procedural grain, not blue noise.
// Shared by display grain and future bounded tile refresh; no texture/history.
inline constexpr char kProceduralScreenNoiseFunctions[]=R"PHOSKIA(
fn ayScreenNoise(x:float,y:float,seed:float):float {
    let low=seed-floor(seed/251.0)*251.0
    let high=floor(seed/251.0)
    let u=x+low
    let v=y+high
    u=u-floor(u/251.0)*251.0
    v=v-floor(v/251.0)*251.0
    let rank=u*67.0+v*131.0
    rank=rank-floor(rank/251.0)*251.0
    rank=rank*rank*3.0+u*v*11.0+u*29.0+v*73.0
    rank=rank-floor(rank/251.0)*251.0
    return (rank+0.5)/251.0
}
)PHOSKIA";
} // namespace ayt::render
