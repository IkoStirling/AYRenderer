#pragma once

#include "AYRenderer/RenderScene.h"
#include "detail/FrameContext.h"

#include <cstdint>

namespace ayt::shader { class ShaderResource; }

namespace ayt::render::detail
{

class ShadowPass;
struct PassExecContext;

// CPU-side light layout shared by deferred LightingPass and both forward
// opaque/transparent material paths. Each array mirrors the Phoskia `Lights` block.
struct PackedSceneLighting {
    alignas(16) float dirs[kMaxSceneLights][4]{};
    alignas(16) float colors[kMaxSceneLights][4]{};
    alignas(16) float params[kMaxSceneLights][4]{};
    alignas(16) float spotDirs[kMaxSceneLights][4]{};
    uint32_t activeLightCount = 0;
};

struct PackedShadowAtlas {
    alignas(16) float rects[kMaxSceneLights][4]{};
    alignas(16) float lightViewProjs[kMaxSceneLights][16]{};
    alignas(16) float biases[kMaxSceneLights][4]{};
    alignas(16) float lightProjectionSlots[kMaxSceneLights][4]{};
    alignas(16) float cascadeSplits[kMaxSceneLights][4]{};
    alignas(16) float cameraForward[4]{0.0f, 0.0f, 1.0f, 0.0f};
    uint32_t activeShadowCount = 0;
    uint32_t activeProjectionCount = 0;
};

PackedSceneLighting packSceneLighting(const SceneLights* sceneLights,
                                      const FrameContext& frame,
                                      bool fallbackWhenEmpty = true) noexcept;

PackedShadowAtlas packShadowAtlas(const ShadowPass* shadowPass,
                                  bool receivesShadow) noexcept;

// Best-effort reflection upload. Old/custom shaders that do not declare the
// array contract keep their legacy single-light uniforms untouched.
bool uploadSceneLighting(ayt::shader::ShaderResource& shader,
                         const PackedSceneLighting& packed);

bool uploadShadowAtlas(ayt::shader::ShaderResource& shader,
                       const PackedShadowAtlas& packed);

// Optional environment bindings and activation flags, shared by material passes.
void bindMaterialEnvironment(ayt::shader::ShaderResource& shader,
                             const PassExecContext& ctx);

} // namespace ayt::render::detail
