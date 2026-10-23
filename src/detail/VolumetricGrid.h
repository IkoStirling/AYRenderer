#pragma once
#include "AYRenderer/VolumetricSettings.h"
#include <bgfx/bgfx.h>
namespace ayt::render::detail {
struct VolumetricGridLayout {
 uint16_t width=0,height=0,atlasWidth=0,atlasHeight=0;
 uint8_t columns=0,rows=0,slices=0;
 uint32_t cells=0;
 bool valid() const noexcept {return width&&height&&columns&&rows&&slices;}
};
inline VolumetricGridLayout makeVolumetricGridLayout(VolumetricSettings settings,uint16_t width,uint16_t height,uint32_t textureLimit) noexcept {
 const auto s=sanitizeVolumetricSettings(settings);if(!s.froxel||!width||!height)return {};
 const uint32_t columns=8,rows=s.froxelSlices/columns,limit=std::min<uint32_t>(textureLimit?textureLimit:16384,65535);
 if(limit<columns||limit<rows)return {};
 uint32_t x=std::min((uint32_t(width)+s.froxelTileSize-1)/s.froxelTileSize,limit/columns);
 uint32_t y=std::min((uint32_t(height)+s.froxelTileSize-1)/s.froxelTileSize,limit/rows);
 const uint64_t maxArea=s.froxelMaxCells/s.froxelSlices;
 if(uint64_t(x)*y>maxArea){const double ratio=std::sqrt(double(maxArea)/double(uint64_t(x)*y));x=std::max(1u,uint32_t(x*ratio));y=std::max(1u,uint32_t(y*ratio));}
 if(uint64_t(x)*y>maxArea){if(x>=y)x=std::max(1u,uint32_t(maxArea/y));else y=std::max(1u,uint32_t(maxArea/x));}
 return {uint16_t(x),uint16_t(y),uint16_t(x*columns),uint16_t(y*rows),uint8_t(columns),uint8_t(rows),s.froxelSlices,uint32_t(uint64_t(x)*y*s.froxelSlices)};
}
inline VolumetricGridLayout runtimeVolumetricGridLayout(const VolumetricSettings& s,uint16_t width,uint16_t height) noexcept {
 if(!s.froxel)return {};const auto* caps=bgfx::getCaps();
 if(!caps||!(caps->formats[bgfx::TextureFormat::RGBA32F]&BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))return {};
 return makeVolumetricGridLayout(s,width,height,caps->limits.maxTextureSize);
}
}
