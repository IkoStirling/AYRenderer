#pragma once

#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Editor selection highlight AFTER PostProcess, BEFORE UI.
//
// Stable path: reuse the same mesh + material draw contract as
// ForwardOpaquePass / TransparentPass (simple_lit_shadow.phoskia, PNT
// layout). No custom Phoskia programs, no offscreen mask RT, no blit,
// no fullscreen triangle — those caused D3D11 setInputLayout / AppVerifier
// hazards when mixed with the deferred pipeline.
//
//   view 17 — alpha-composited selection mesh onto the post-processed
//             Game View backbuffer rect (DEPTH_TEST_ALWAYS).
class EditorOverlayPass : public RenderPass {
public:
    static constexpr uint8_t kMaskViewId  = 16;  // reserved (ABI / tests)
    static constexpr uint8_t kBlitViewId  = 17;

    std::string_view name() const override { return "EditorOverlay"; }

    uint32_t execute(PassExecContext& ctx) override;

private:
    static bool submitOutlineItem(BGFXAdapter& adapter,
                                  PassExecContext& ctx,
                                  const FrameContext& frame,
                                  const DrawItem& item,
                                  uint8_t viewId);
};

} // namespace ayt::render::detail
