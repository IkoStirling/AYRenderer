#pragma once
#include <algorithm>
#include <cmath>
namespace ayt::render::detail {
struct SpotShadowProjection {float nearZ,farZ,fov,tanHalfFov;};
/// One source of truth for atlas projection and PCSS depth reconstruction.
inline SpotShadowProjection spotShadowProjection(float coneCosOuter,float range) noexcept {
    constexpr float pi=3.14159265358979323846f;
    const float cosine=std::clamp(std::isfinite(coneCosOuter)?coneCosOuter:.5f,-.996f,.9999f);
    const float fov=std::clamp(2.f*std::acos(cosine),pi/180,pi*(175.f/180));
    const float farZ=std::max(std::isfinite(range)?range:1.f,.1f);
    const float nearZ=std::min(std::max(farZ*.01f,.025f),farZ*.5f);
    return {nearZ,farZ,fov,std::tan(fov*.5f)};
}
/// ShadowCaster stores .5+.5*(clipZ/clipW); LH clipZ/clipW is0..1.
inline float spotShadowDistance(float encoded,float nearZ,float farZ) noexcept {
    return nearZ*farZ/std::max(farZ-(encoded*2-1)*(farZ-nearZ),1e-8f);
}
} // namespace ayt::render::detail
