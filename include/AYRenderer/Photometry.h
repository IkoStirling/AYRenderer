#pragma once
#include <cstdint>
#include <span>
#include <AYRenderer/RenderScene.h>
#include <string>
#include <string_view>
#include <vector>
namespace ayt::render {
/// Legacy preserves authored engine units; physical modes assume one world unit
/// is one meter. Color remains a linear RGB multiplier (white has luminance1).
enum class LightIntensityUnit : uint8_t { Legacy, Candela, Lumen };
/// Validated LM-63 Type C, TILT=NONE photometry. Immutable after parse; samples
/// are horizontal-major absolute candelas, including the file multiplier.
class IesProfile final {
public:
    /// Supports LM-63-1995/2002/2019, Type C, rotational/quadrant/bilateral/full
    /// symmetry. Rejects external/included TILT and malformed/oversized data.
    /// Transactional; no I/O, maximum4MiB and262144 measured samples.
    static bool parse(std::string_view text,IesProfile& output,std::string* error=nullptr);
    float sample(float verticalDegrees,float horizontalDegrees) const noexcept;
    float peakCandela() const noexcept { return _peak; }
    /// Integral of the peak-normalized IES times the engine's core/field
    /// smoothstep. Numerical quadrature in cosine space; iris/Gobo are excluded.
    float coneSolidAngle(float cosInner,float cosOuter) const noexcept;
    /// Project normalized distribution onto a finite perspective Gobo plane.
    /// Host uploads linear RGBA8 (256 default); use same outer half-angle as
    /// the light and re-bake on zoom. The finite exit is a near-field approximation.
    bool projection(float outerAngle,uint16_t size,std::vector<uint8_t>& rgba) const;
    /// Fixed257x129 angular atlas with duplicated360deg seam, linear RGBA8.
    bool angularTexture(std::vector<uint8_t>& rgba) const;
private:
    std::vector<float> _vertical,_horizontal,_candela,_mean;
    float _peak=0;
};
class Renderer;
/// Upload a Renderer-owned linear IES atlas. The host destroys the handle before
/// shutdown and keeps it live through dispatch; no implicit file I/O.
TextureHandle createIesTexture(Renderer& renderer,const IesProfile& profile);
/// Smoothstep Spot effective solid angle in steradians. Zero/invalid gives0.
float spotSolidAngle(float cosInner,float cosOuter) noexcept;
/// On-axis illuminance before range fade/occlusion. Inputs in cd and meters;
/// cosine is the receiving surface angle. Invalid/nonpositive distance gives0.
float illuminanceLux(float candela,float distanceMeters,float cosine=1) noexcept;
} // namespace ayt::render
