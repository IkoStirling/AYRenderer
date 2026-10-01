#pragma once
#include <AYMath/MathTypes.h>
namespace ayt::render {
/// Camera owned by an isolated authoring viewport; never modifies the main camera.
struct PreviewSceneCamera {
    math::Float4x4 view = math::Float4x4::identity();
    math::Float4x4 projection = math::Float4x4::identity();
    math::FVector3 position{};
};
}
