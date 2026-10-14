#pragma once
#include "detail/RenderViewOrder.h"
#include <cstdint>

namespace ayt::render::detail {
inline constexpr uint16_t kCameraFirstView = 64;
inline constexpr uint16_t kCameraLastView = 111;
// Offscreen UI may consume only views absent from the fixed scene/editor order
// and the runtime camera range. Grouped previews must not straddle a reservation.
inline constexpr uint16_t nextUiViewRange(uint16_t next, uint16_t count) noexcept {
    if (!count) return UINT16_MAX;
    while (uint32_t(next) + count - 1 <= 242) {
        const uint32_t last = uint32_t(next) + count - 1;
        if (next <= kCameraLastView && last >= kCameraFirstView) { next=kCameraLastView+1; continue; }
        bool reserved=false;
        for (auto view : kRenderViewOrder) if (view >= next && view <= last) {
            next=uint16_t(view+1); reserved=true; break;
        }
        if (!reserved) return next;
    }
    return UINT16_MAX;
}
inline constexpr uint16_t uiViewCapacity() noexcept {
    uint16_t count=0;
    for(uint16_t next=32; next<=242;) {
        next=nextUiViewRange(next,1); if(next==UINT16_MAX) break;
        ++count; ++next;
    }
    return count;
}
}
