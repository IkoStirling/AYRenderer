#pragma once
#include <AYRenderer/CameraViews.h>
namespace ayt::render {
class Renderer;
namespace detail {
class BGFXAdapter;
// Native-device acceptance access only; never a gameplay acquisition path.
struct CameraViewsTestAccess {
    static uint16_t framebuffer(const Renderer& renderer, CameraOutputHandle output) noexcept;
    static BGFXAdapter* adapter(Renderer& renderer) noexcept;
};
}
}
