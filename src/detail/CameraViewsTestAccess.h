#pragma once
#include <AYRenderer/CameraViews.h>
namespace ayt::render {
class Renderer;
namespace detail {
class BGFXAdapter;
// Native-device acceptance access only; never a gameplay acquisition path.
struct CameraViewsTestAccess {
    static uint16_t framebuffer(const Renderer& renderer, CameraOutputHandle output) noexcept;
    static uint16_t finalColor(Renderer& renderer) noexcept;
    static BGFXAdapter* adapter(Renderer& renderer) noexcept;
};
}
}
