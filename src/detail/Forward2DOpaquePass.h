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

// CM-1 (2026-08-11) — 2D lane pass. Draws every DrawItem carrying a
// non-null `payload` (DrawPayload2D) with:
//   - alpha blend ON (BGFX_STATE_BLEND_ALPHA), NO depth test, NO depth
//     write — 2D lives at ortho z=0; depth-testing same-z draws would
//     discard later submissions. The CPU-side packedSortKey order IS
//     the final order (AY2D design.md §7.4 stable_sort semantics).
//     This is the pass's biggest difference from ForwardOpaquePass.
//   - per-draw uniforms srcRect / tint / flip uploaded from the
//     payload (missing bindings are silent no-ops).
//   - albedo textures bound with the same loop shape as
//     ForwardOpaquePass::flushMaterial; "shadowMap" slots skipped
//     (2D has no shadow path).
// Materials MUST stay BlendMode::Opaque — an Alpha 2D material would
// ALSO be submitted by TransparentPass (double draw). The pass does
// not read blendMode; the Opaque contract is the caller's.
class Forward2DOpaquePass : public RenderPass {
public:
    std::string_view name() const override { return "Forward2DOpaque"; }

    uint32_t execute(PassExecContext& ctx) override;
};

} // namespace ayt::render::detail
