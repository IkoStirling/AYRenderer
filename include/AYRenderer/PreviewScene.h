#pragma once
#include <AYMath/MathTypes.h>
#include <cstdint>
namespace ayt::render {
struct SceneLights;
/// Camera owned by an isolated authoring viewport; never modifies the main camera.
struct PreviewSceneCamera {
    math::Float4x4 view = math::Float4x4::identity();
    math::Float4x4 projection = math::Float4x4::identity();
    math::FVector3 position{};
};
enum class PreviewToneMap : uint8_t { None, Reinhard, ACES };
/// Optional isolated authoring settings; no main camera/light/post state changes.
/// lights is borrowed only during the synchronous preview call. Defaults keep
/// the original raw four-view preview. Display transform uses a separate HDR
/// target and a fifth view; it applies the same shader as main PostProcess.
/// Exposure/gamma use the main post-process finite ranges (0..64/.1..8).
/// Non-finite values use 1/2.2; unknown tone maps use None. Non-finite light
/// vectors use defaults. A non-finite camera rejects the call before submission.
struct PreviewSceneSettings {
    math::FVector3 lightDirection{.3f,-.8f,-.4f};
    math::FVector3 lightColor{1,1,1};
    const SceneLights* lights=nullptr;
    bool displayTransform=false;
    float exposure=1,gamma=2.2f;
    PreviewToneMap toneMap=PreviewToneMap::None;
};
}
