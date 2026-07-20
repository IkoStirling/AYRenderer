#pragma once

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace ayt::render::detail
{

// U0 — first concrete RenderPass subclass. behavior unchanged; the
// only differences are:
//   (1) inherits from detail::RenderPass,
//   (2) name() override returns "ForwardOpaque",
//   (3) execute() carries `override` keyword so MSVC catches
//       signature drift if the base class method changes.
//
// kMainViewId remains on this subclass — UI / Transparent /
// PostProcess subclasses will define their own view ids in U1+.
// Default view id = 0 still means "main 3D scene"; the
// composite-mode override (view 1) flows through execute()'s
// `viewId` arg from Renderer::render.
class ForwardOpaquePass : public RenderPass {
private:
    static void flushMaterial(GpuMaterial& material,
                              const std::unordered_map<uint64_t, GpuTexture>& textures,
                              const FrameContext& frame,
                              const ayt::math::Float4x4& world);

public:
    static constexpr uint8_t kMainViewId = 0;

    std::string_view name() const override { return "ForwardOpaque"; }

    uint32_t execute(PassExecContext& ctx) override;
};

} // namespace ayt::render::detail
