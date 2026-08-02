#pragma once

#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "AYRenderTypes.h"  // ayt::render::BlendMode

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace ayt::render::detail
{

// U1 — third concrete RenderPass subclass. Filters scene.items()
// for `material.blendMode == BlendMode::Alpha` and submits them with
// BGFX_STATE_BLEND_ALPHA so the alpha-blended geometry composites
// over the opaque result.
//
// View id: same viewId as ForwardOpaquePass. We share the 3D view
// between opaque and transparent; bgfx draws submit in the order we
// call them, so the ForwardOpaque dispatch must run first (to
// populate the depth buffer), then the Transparent dispatch reuses
// that depth for STATE_DEPTH_TEST_LESS but never writes its own Z.
//
// Sort: NOT implemented in U1 (design.md:460 says "blend state +
// 排序" — back-to-front sort is deferred to U1+ when a sort-key
// field lands on DrawItem). Today insertion order is honored; users
// who need correct alpha ordering without scene-level sort should
// add `scene.add(...)` calls back-to-front manually.
//
// Color override support mirrors ForwardOpaquePass::flushMaterial
// lines 69-79 (without the skinning branch — U1 transparent draws
// are non-skinned; skinned transparent will revisit).
class TransparentPass : public RenderPass {
public:
    std::string_view name() const override { return "Transparent"; }

    uint32_t execute(PassExecContext& ctx) override;

private:
    static bool submitItem(BGFXAdapter& adapter,
                           PassExecContext& ctx,
                           const FrameContext& frame,
                           const DrawItem& item,
                           uint8_t viewId,
                           const ayt::math::Float4x4* worldOverride = nullptr);
};

} // namespace ayt::render::detail
