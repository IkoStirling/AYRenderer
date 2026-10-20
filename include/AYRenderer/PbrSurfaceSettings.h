#pragma once

namespace ayt::render
{

/// StandardLit surface controls. UVs are shared by every surface/opacity map,
/// including cutout depth, shadow and motion replay. Values are copied on the
/// Renderer owner thread; defaults preserve existing PBR materials.
struct PbrSurfaceSettings
{
    float uvScaleX = 1, uvScaleY = 1;   // .01..128; positive repeat density.
    float uvOffsetX = 0, uvOffsetY = 0; // -1024..1024 source UV units.
    float normalStrength = 1;           // 0..4; zero selects the geometric normal.
    float roughnessBias = 0;            // -1..1, added after factor * texture, before clamp.
    bool operator==(const PbrSurfaceSettings &) const = default;
};

/// Clamp finite controls; replace non-finite values with identity defaults.
PbrSurfaceSettings sanitizePbrSurface(PbrSurfaceSettings settings) noexcept;

} // namespace ayt::render
