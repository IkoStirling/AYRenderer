#include <AYRenderer/VolumetricDensity.h>
#include <AYRenderer.h>
#include <algorithm>
#include <cmath>
namespace ayt::render {
std::shared_ptr<const VolumetricDensityField> VolumetricDensityField::create(uint16_t w,uint16_t h,uint16_t d,std::span<const uint8_t> values,std::string* error){
 if(!w||!h||!d||w>64||h>64||d>64||values.size()!=size_t(w)*h*d){if(error)*error="Invalid density grid dimensions/count";return {};}
 if(error)error->clear();return std::shared_ptr<const VolumetricDensityField>(new VolumetricDensityField(w,h,d,values));
}
float VolumetricDensityField::sample(const math::FVector3& p) const noexcept {
 for(int axis=0;axis<3;++axis)if(!std::isfinite(p[axis])||std::abs(p[axis])>1)return 0;
 const float x=(p.x*.5f+.5f)*(_width-1),y=(p.y*.5f+.5f)*(_height-1),z=(p.z*.5f+.5f)*(_depth-1);
 const auto ix=uint16_t(x),iy=uint16_t(y),iz=uint16_t(z);const float fx=x-ix,fy=y-iy,fz=z-iz;
 float sum=0;for(int dz=0;dz<2;++dz)for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){const auto vx=std::min<uint16_t>(ix+dx,_width-1),vy=std::min<uint16_t>(iy+dy,_height-1),vz=std::min<uint16_t>(iz+dz,_depth-1);const float weight=(dx?fx:1-fx)*(dy?fy:1-fy)*(dz?fz:1-fz);sum+=weight*_values[vx+_width*(vy+_height*vz)]/255.f;}return sum;
}
std::vector<uint8_t> VolumetricDensityField::atlas() const {
 std::vector<uint8_t> rgba(size_t(atlasWidth())*atlasHeight()*4,0);
 for(uint16_t z=0;z<_depth;++z)for(uint16_t y=0;y<_height;++y)for(uint16_t x=0;x<_width;++x){const auto at=4*(size_t((z/8)*_height+y)*atlasWidth()+(z%8)*_width+x);const auto value=_values[x+_width*(y+_height*z)];rgba[at]=rgba[at+1]=rgba[at+2]=value;rgba[at+3]=255;}return rgba;
}
TextureHandle createVolumetricDensityTexture(Renderer& renderer,const VolumetricDensityField& profile){const auto rgba=profile.atlas();return renderer.createTextureFromRgba8(profile.atlasWidth(),profile.atlasHeight(),rgba.data());}
}
