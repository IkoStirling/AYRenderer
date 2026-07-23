#include "detail/ShadowAtlas.h"

#include <algorithm>
#include <cmath>

namespace ayt::render::detail
{

ShadowAtlasLayout computeShadowAtlasLayout(const ShadowAtlasConfig& cfg)
{
    ShadowAtlasLayout out{};
    out.atlasSize = cfg.atlasSize;

    // Clamp slotCount to a valid range. slotCount==0 ⇒ empty
    // layout (used as a sentinel by callers that want to disable
    // per-light shadow without touching the rest of the pipeline).
    const uint32_t n = std::clamp(cfg.slotCount,
                                  uint32_t{0},
                                  kShadowAtlasMaxSlots);
    out.slotCount = n;
    if (n == 0) {
        return out;  // subRects stays all-zero, gridCols/gridRows stay 0
    }

    // Pick a roughly-square grid: rows = ceil(sqrt(N)),
    // cols = ceil(N / rows). This minimizes aspect-ratio skew on
    // the sub-rects (cube-like) and keeps the shader's atlas
    // UV math readable. Worked examples:
    //   N=1 → rows=1, cols=1
    //   N=2 → rows=1, cols=2
    //   N=4 → rows=2, cols=2
    //   N=8 → rows=2, cols=4  ← §P5.5 C default
    const float nF = static_cast<float>(n);
    const uint32_t rows = static_cast<uint32_t>(
        std::ceil(std::sqrt(nF)));
    const uint32_t cols = static_cast<uint32_t>(
        std::ceil(nF / static_cast<float>(rows)));
    out.gridRows = rows;
    out.gridCols = cols;

    const float invCols = 1.0f / static_cast<float>(cols);
    const float invRows = 1.0f / static_cast<float>(rows);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t col = i % cols;
        const uint32_t row = i / cols;
        // Atlas is sampled with V growing downward in image
        // space, but bgfx::createTexture + bgfx::setViewRect use
        // top-left origin. We invert the row here so slot[0]
        // lands in the top-left tile and slot[N-1] in the
        // bottom-right tile — matches the natural reading order.
        const uint32_t yIdx = row;
        const float u0 = static_cast<float>(col)     * invCols;
        const float v0 = static_cast<float>(yIdx)    * invRows;
        const float u1 = static_cast<float>(col + 1) * invCols;
        const float v1 = static_cast<float>(yIdx + 1)* invRows;
        out.subRects[i][0] = u0;
        out.subRects[i][1] = v0;
        out.subRects[i][2] = u1;
        out.subRects[i][3] = v1;
    }
    return out;
}

ShadowAtlasPixelRect shadowAtlasSlotPixelRect(const ShadowAtlasLayout& layout,
                                              uint32_t slot)
{
    if (slot >= layout.slotCount
        || layout.gridCols == 0
        || layout.gridRows == 0) {
        return ShadowAtlasPixelRect{0, 0, 0, 0};
    }
    const uint32_t cols = layout.gridCols;
    const uint32_t rows = layout.gridRows;
    const uint32_t col  = slot % cols;
    const uint32_t row  = slot / cols;
    const uint32_t slotW = layout.atlasSize / cols;
    const uint32_t slotH = layout.atlasSize / rows;
    return ShadowAtlasPixelRect{
        static_cast<uint16_t>(col * slotW),
        static_cast<uint16_t>(row * slotH),
        static_cast<uint16_t>(slotW),
        static_cast<uint16_t>(slotH),
    };
}

} // namespace ayt::render::detail
