#pragma once

#include <cstddef>
#include <cstdint>

namespace ayt::render::detail {
inline constexpr std::size_t kRenderViewCapacity=256;
inline constexpr uint16_t kCameraFirstView=64;
inline constexpr uint16_t kCameraLastView=111;
inline constexpr uint16_t kCameraReadbackView=112;
inline constexpr uint16_t kDefaultUiView=255;

constexpr bool canReserveMainRenderView(uint16_t view) noexcept
{
    return view<kRenderViewCapacity && view!=kDefaultUiView
        && !(view>=kCameraFirstView && view<=kCameraLastView)
        && view!=kCameraReadbackView;
}
}
