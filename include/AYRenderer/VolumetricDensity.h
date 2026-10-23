#pragma once
#include <AYRenderer/RenderTypes.h>
#include <AYMath/MathTypes.h>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace ayt::render {
class Renderer;
/// Immutable normalized extinction multipliers in a local axis-aligned medium.
/// X-major uint8 voxels, 1..64 per axis, trilinear CPU sampling, zero outside [-1,1]^3.
class VolumetricDensityField final {
public:
    /// Copies at most262144 cells once. Invalid dimensions/count return null, no GPU/I/O.
    static std::shared_ptr<const VolumetricDensityField> create(uint16_t width,uint16_t height,uint16_t depth,std::span<const uint8_t> values,std::string* error=nullptr);
    float sample(const math::FVector3& localPosition) const noexcept;
    uint16_t width() const noexcept {return _width;}
    uint16_t height() const noexcept {return _height;}
    uint16_t depth() const noexcept {return _depth;}
    uint16_t atlasWidth() const noexcept {return _width*8;}
    uint16_t atlasHeight() const noexcept {return _height*((_depth+7)/8);}
    /// Eight-column slice atlas, linear RGBA8; red carries density, alpha is opaque.
    std::vector<uint8_t> atlas() const;
private:
    VolumetricDensityField(uint16_t w,uint16_t h,uint16_t d,std::span<const uint8_t> values):_width(w),_height(h),_depth(d),_values(values.begin(),values.end()){}
    uint16_t _width,_height,_depth;std::vector<uint8_t> _values;
};
struct VolumetricDensityBinding {std::shared_ptr<const VolumetricDensityField> profile;TextureHandle texture;};
/// Upload the matching linear atlas to this Renderer; retain it through rendering.
TextureHandle createVolumetricDensityTexture(Renderer& renderer,const VolumetricDensityField& profile);
}
