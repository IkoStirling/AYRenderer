#include "AYTest.h"
#include "AYRenderer/RenderTypes.h"

#include "detail/EditorOverlayPass.h"
#include "detail/PostProcessPass.h"

#include <algorithm>
#include <cstdint>

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;

TEST_SUITE(AYRenderer_EditorOverlay)

TEST_CASE(editoroverlay_slot_abi_is_13) {
    CHECK(static_cast<uint8_t>(RenderPassSlot::EditorOverlay) == 13u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::GBufferDebug) == 12u);
}

TEST_CASE(editoroverlay_view_id_between_post_and_ui) {
    CHECK(ayt::render::detail::EditorOverlayPass::kMaskViewId == 16u);
    CHECK(ayt::render::detail::EditorOverlayPass::kBlitViewId == 17u);
    CHECK(ayt::render::detail::PostProcessPass::kBlitViewId == 15u);
}

TEST_CASE(make_default_omits_editoroverlay) {
    const RenderPipelineDesc def = RenderPipelineDesc::makeDefault();
    CHECK(!def.contains(RenderPassSlot::EditorOverlay));
}

TEST_CASE(make_editor_forward_inserts_overlay_after_postprocess) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeEditorForward();
    CHECK(desc.contains(RenderPassSlot::EditorOverlay));
    CHECK(desc.passes.size() == 10u);  // 9-slot default (CM-1 2026-08-11) + EditorOverlay
    CHECK(desc.passes[3] == RenderPassSlot::Transparent);
    CHECK(desc.passes[6] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[7] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[8] == RenderPassSlot::EditorOverlay);
    CHECK(desc.passes[9] == RenderPassSlot::UI);
}

TEST_CASE(make_editor_deferred_inserts_overlay_after_postprocess) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeEditorDeferred();
    CHECK(desc.contains(RenderPassSlot::EditorOverlay));
    const auto ppIt = std::find(desc.passes.begin(), desc.passes.end(),
                                RenderPassSlot::PostProcess);
    CHECK(ppIt != desc.passes.end());
    CHECK(ppIt + 1 != desc.passes.end());
    CHECK(*(ppIt + 1) == RenderPassSlot::EditorOverlay);
    CHECK(desc.contains(RenderPassSlot::GBufferDebug));
}

TEST_SUITE_END
