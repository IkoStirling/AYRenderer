#pragma once

#include "AYRenderer/RenderScene.h"
#include "AYShader/ShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ayt::render::detail
{

// Produces the final painter-order list shared by all 2D scene producers.
std::vector<const DrawItem*> collectSortedOverlay2DItems(
    const RenderScene& scene);

// CM-1 (2026-08-11) — 2D lane pass. Draws every DrawItem carrying a
// non-null `payload` (DrawPayload2D) with:
//   - RenderScene's independent overlay camera when present; the 3D main
//     camera remains untouched.
//   - alpha blend ON, NO depth test/write. The lane is a camera overlay and
//     executes after all 3D transparency. World-space depth-aware 2D is a
//     separate future domain.
//   - one global stable sort by packedSortKey across sprite and tilemap
//     producers (AY2D design.md §7.4 semantics).
//   - per-draw uniforms srcRect / tint / flip uploaded from the
//     payload (missing bindings are silent no-ops).
//   - albedo textures bound with the same loop shape as
//     ForwardOpaquePass::flushMaterial; "shadowMap" slots skipped
//     (2D has no shadow path).
// 3D passes explicitly reject payload items, so custom 2D material blend
// metadata cannot cause a second submission.
class Forward2DOpaquePass : public RenderPass {
public:
    // View 246 is reserved from the retained-UI offscreen range. Keeping this
    // pass on a dedicated view prevents collisions with Forward view 0/3 and
    // Deferred MotionVector view 3.
    static constexpr uint8_t kOverlayViewId = 246;

    std::string_view name() const override { return "Forward2DOpaque"; }

    uint32_t execute(PassExecContext& ctx) override;
};

} // namespace ayt::render::detail
