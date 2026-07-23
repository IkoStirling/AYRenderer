#pragma once

// §P5.5 C (2026-07-23) — Per-light shadow atlas layout.
// One large RGBA8 depth-equivalent texture (atlasSize x atlasSize)
// is split into a 2D grid of up to kMaxSlots sub-rects. Each
// sub-rect hosts one light's shadow caster output. LightingPass FS
// uses shadowAtlasRects[i] to compute UV within the atlas when
// sampling shadowMap for light slot i.
//
// Default layout (kDefaultGridCols=4 x kDefaultGridRows=2 = 8 slots,
// each 2048x2048 if atlasSize=4096). The grid is auto-derived from
// requestedSlots: rows = ceil(sqrt(N)), cols = ceil(N / rows).
//
// Scope:
//   - Directional + Spot each occupy one sub-rect (single-VP caster).
//   - Point omni-shadow is OUT OF SCOPE for §P5.5 C.
//   - ShadowPass uses BGFXAdapter scissor state to clip each caster
//     pass to its sub-rect (single view id 1 reused, see cutsheet
//     §设计决定 #6 — scissor not multi-view).

#include <cstdint>

namespace ayt::render::detail
{

constexpr uint32_t kShadowAtlasMaxSlots = 8;        // matches kMaxSceneLights
constexpr uint32_t kShadowAtlasDefaultSize = 4096;  // 4096 x 4096 = 16 MB RGBA8
constexpr uint32_t kShadowAtlasDefaultSlots = 8;

// §P5.5 C — atlas sub-rect layout. Auto-derived grid splits the
// atlas into roughly-square sub-rects so shadow map texel coverage
// stays uniform across all caster slots.
//
// UV convention: subRects[i] = (u0, v0, u1, v1) in atlas UV space
// [0,1]. LightingPass FS computes per-light shadow UV as
//   mix(slotRect.xy, slotRect.zw, atlasLocalUv)
// where atlasLocalUv is the light-space-projected UV in [0,1] for
// this fragment. Unused slots get rect (0,0,0,0) — never sampled
// because perLightShadowCount gates the active range.
struct ShadowAtlasLayout {
    float     subRects[kShadowAtlasMaxSlots][4]{};   // (u0, v0, u1, v1) per slot
    uint32_t  slotCount = 0;                          // effective slot count
    uint32_t  gridCols  = 0;                          // derived columns
    uint32_t  gridRows  = 0;                          // derived rows
    uint32_t  atlasSize = 0;                          // echo of the input
};

// §P5.5 C — atlas config POD. Hosts pass this to ShadowPass to
// override defaults (currently used only by tests for sizing
// verification). Production pipeline uses the ShadowPass defaults
// (4096 / 8) via ShadowPass::setAtlasConfig().
struct ShadowAtlasConfig {
    uint32_t atlasSize = kShadowAtlasDefaultSize;
    uint32_t slotCount = kShadowAtlasDefaultSlots;
};

// §P5.5 C — compute a ShadowAtlasLayout for the given config.
// Algorithm:
//   1. clamp slotCount to [1, kShadowAtlasMaxSlots]
//   2. gridRows = ceil(sqrt(N)), gridCols = ceil(N / gridRows)
//   3. each sub-rect = (col * (1/cols), row * (1/rows),
//                        (col+1) * (1/cols), (row+1) * (1/rows))
// Atlas is square — atlasSize is recorded verbatim so ShadowPass
// can size its FBO without recomputing. Returns a layout with
// slotCount=0 if slotCount==0 (no-op layout).
ShadowAtlasLayout computeShadowAtlasLayout(const ShadowAtlasConfig& cfg);

// §P5.5 C — small helper: derive atlas pixel rect (x, y, w, h) for
// slot i, given the same layout metadata. Used by ShadowPass to
// drive BGFXAdapter::setScissorRect per caster pass. Returns
// (0,0,0,0) for slots >= slotCount.
struct ShadowAtlasPixelRect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
};
ShadowAtlasPixelRect shadowAtlasSlotPixelRect(const ShadowAtlasLayout& layout,
                                              uint32_t slot);

} // namespace ayt::render::detail
