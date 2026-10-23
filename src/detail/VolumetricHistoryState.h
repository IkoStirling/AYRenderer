#pragma once
#include "detail/VolumetricGrid.h"
#include "detail/FrameContext.h"
#include <array>
#include <cmath>
namespace ayt::render::detail {
// CPU submission history, independent of TAA/exposure and GPU timing latency.
struct VolumetricHistoryState {
 bool valid=false;
 FrameContext previous;
 VolumetricGridLayout grid;
 uint32_t frame=0;
 void invalidate() noexcept {valid=false;}
 static std::array<float,18> signature(const VolumetricSettings& s) noexcept {
  return {s.density,s.scattering,s.anisotropy,s.maxDistance,s.local?1.f:0.f,s.center.x,s.center.y,s.center.z,s.halfExtent.x,s.halfExtent.y,s.halfExtent.z,s.tint.x,s.tint.y,s.tint.z,s.noiseStrength,s.noiseScale,float(s.froxelTileSize),float(s.froxelMaxCells)};
 }
 bool reusable(const FrameContext& now,const VolumetricGridLayout& layout,uint32_t counter) const noexcept {
  if(!valid||counter!=uint32_t(frame+1)||layout.width!=grid.width||layout.height!=grid.height||layout.slices!=grid.slices||signature(now.volumetric)!=signature(previous.volumetric))return false;
  if(now.densityField.profile!=previous.densityField.profile||now.densityField.texture.id!=previous.densityField.texture.id)return false;
  const float dt=now.volumetric.noiseTime-previous.volumetric.noiseTime;
  if(dt<0||dt>.25f||(now.cameraPosition-previous.cameraPosition).length()>4.f)return false;
  for(int i=0;i<16;++i)if(std::abs(now.projection.ptr()[i]-previous.projection.ptr()[i])>.05f)return false;
  for(int y=0;y<3;++y)for(int x=0;x<3;++x)if(std::abs(now.view.row[y][x]-previous.view.row[y][x])>.35f)return false;
  return true;
 }
 void commit(const FrameContext& now,const VolumetricGridLayout& layout,uint32_t counter){previous=now;grid=layout;frame=counter;valid=true;}
};
}
