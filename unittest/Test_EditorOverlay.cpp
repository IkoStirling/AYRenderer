#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderTypes.h"

#include "detail/EditorOverlayPass.h"
#include "detail/PostProcessPass.h"
#include "detail/PresentPass.h"

#include "AYShader/ShadercDriver.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/stat.h>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::Renderer;
using ayt::render::EditorTransformGizmoMode;
using ayt::render::EditorTransformGizmoState;

namespace {

bool editorOverlayFileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

} // namespace

TEST_SUITE(AYRenderer_EditorOverlay)

TEST_CASE(editoroverlay_slot_abi_is_13) {
    CHECK(static_cast<uint8_t>(RenderPassSlot::EditorOverlay) == 13u);
    CHECK(static_cast<uint8_t>(RenderPassSlot::GBufferDebug) == 12u);
}

TEST_CASE(editoroverlay_view_id_between_post_and_ui) {
    CHECK(ayt::render::detail::EditorOverlayPass::kAxisViewId == 251u);
    CHECK(ayt::render::detail::EditorOverlayPass::kMaskViewId == 251u);
    CHECK(ayt::render::detail::EditorOverlayPass::kBlitViewId == 252u);
    CHECK(ayt::render::detail::PostProcessPass::kBlitViewId == 15u);
    CHECK(ayt::render::detail::PresentPass::kPresentViewId == 16u);
}

TEST_CASE(editoroverlay_orientation_axis_is_explicit_editor_opt_in) {
    ayt::render::detail::EditorOverlayPass pass;
    CHECK_FALSE(pass.orientationAxisEnabled());
    pass.setOrientationAxisEnabled(true);
    CHECK(pass.orientationAxisEnabled());

    ayt::render::Renderer renderer;
    CHECK_FALSE(renderer.viewportOrientationAxisEnabled());
    renderer.setViewportOrientationAxisEnabled(true);
    CHECK(renderer.viewportOrientationAxisEnabled());
    renderer.configurePipeline(RenderPipelineDesc::makeEditorDeferred());
    CHECK(renderer.viewportOrientationAxisEnabled());
}

TEST_CASE(editoroverlay_transform_gizmo_is_explicit_state_and_uses_view_252) {
    ayt::render::detail::EditorOverlayPass pass;
    CHECK_FALSE(pass.transformGizmoState().visible);
    CHECK(pass.transformGizmoState().mode
          == EditorTransformGizmoMode::Hidden);
    CHECK(static_cast<uint8_t>(EditorTransformGizmoMode::Universal) == 4u);
    CHECK(ayt::render::detail::EditorOverlayPass::kGizmoViewId == 252u);

    EditorTransformGizmoState state;
    state.visible = true;
    state.localSpace = true;
    state.mode = EditorTransformGizmoMode::Universal;
    state.activeHandle = 8u;
    state.disabledHandleMask = static_cast<uint16_t>((1u << 3) | (1u << 12));
    state.position = {1.0f, 2.0f, 3.0f};
    pass.setTransformGizmoState(state);
    CHECK(pass.transformGizmoState().visible);
    CHECK(pass.transformGizmoState().localSpace);
    CHECK(pass.transformGizmoState().activeHandle == 8u);
    CHECK(pass.transformGizmoState().disabledHandleMask
          == state.disabledHandleMask);
    CHECK(pass.transformGizmoState().position.y == 2.0f);
}

TEST_CASE(renderer_transform_gizmo_state_survives_pipeline_rebuild) {
    Renderer renderer;
    EditorTransformGizmoState state;
    state.visible = true;
    state.mode = EditorTransformGizmoMode::Universal;
    state.activeHandle = 4u;
    state.disabledHandleMask = static_cast<uint16_t>(1u << 2);
    state.position = {3.0f, 4.0f, 5.0f};
    renderer.setEditorTransformGizmoState(state);
    renderer.configurePipeline(RenderPipelineDesc::makeEditorDeferred());
    const EditorTransformGizmoState restored =
        renderer.editorTransformGizmoState();
    CHECK(restored.visible);
    CHECK(restored.mode == EditorTransformGizmoMode::Universal);
    CHECK(restored.activeHandle == 4u);
    CHECK(restored.disabledHandleMask == state.disabledHandleMask);
    CHECK(restored.position.z == 5.0f);
}

TEST_CASE(editoroverlay_orientation_axis_view_keeps_rotation_not_translation) {
    ayt::math::Float4x4 sceneView = ayt::math::Float4x4::identity();
    sceneView.row[0] = ayt::math::FVector4(0.0f, 0.0f, 1.0f, 41.0f);
    sceneView.row[1] = ayt::math::FVector4(0.0f, 1.0f, 0.0f, -12.0f);
    sceneView.row[2] = ayt::math::FVector4(-1.0f, 0.0f, 0.0f, 7.0f);

    const ayt::math::Float4x4 axisView =
        ayt::render::detail::EditorOverlayPass::makeOrientationAxisView(
            sceneView);

    CHECK(axisView.row[0].x == sceneView.row[0].x);
    CHECK(axisView.row[0].z == sceneView.row[0].z);
    CHECK(axisView.row[1].y == sceneView.row[1].y);
    CHECK(axisView.row[2].x == sceneView.row[2].x);
    CHECK(axisView.row[0].w == 0.0f);
    CHECK(axisView.row[1].w == 0.0f);
    CHECK(axisView.row[2].w == 3.0f);
    CHECK(axisView.row[3].w == 1.0f);
}

TEST_CASE(editoroverlay_orientation_axis_shader_compiles_for_d3d11_and_d3d12) {
    if (!editorOverlayFileExists(AY_SHADER_SHADERC_HINT)) {
        std::cerr << "[EditorOverlay test] SKIP: shaderc unavailable.\n";
        return;
    }

    ayt::shader::AYShadercDriver driver(AY_SHADER_SHADERC_HINT);
    auto compileStage = [&](const char* stage, const char* source) {
        ayt::shader::ShaderCompileRequest request;
        request.stage = stage;
        request.scSource = source;
        request.varyingdefSource =
            ayt::render::detail::EditorOverlayPass::orientationAxisVaryingDef();
        request.platform = "windows";
        // bgfx's D3D11 and D3D12 backends both consume its s_5_0 output.
        request.profile = "s_5_0";
        request.outputName = std::string("editor_orientation_axis_") + stage;
        request.timeoutMs = 30000;
#ifdef AY_SHADER_BGFX_COMMON_HINT
        request.includeDirs.emplace_back(AY_SHADER_BGFX_COMMON_HINT);
#endif
#ifdef AY_SHADER_BGFX_SRC_HINT
        request.includeDirs.emplace_back(AY_SHADER_BGFX_SRC_HINT);
#endif
        const ayt::shader::ShaderCompileResult result = driver.compile(request);
        if (!result.ok) {
            std::cerr << "[EditorOverlay test] shaderc " << stage
                      << " failed: " << result.stderrText << '\n';
        }
        return result.ok;
    };

    CHECK(compileStage(
        "vertex",
        ayt::render::detail::EditorOverlayPass::orientationAxisVertexShader()));
    CHECK(compileStage(
        "fragment",
        ayt::render::detail::EditorOverlayPass::orientationAxisFragmentShader()));
}

TEST_CASE(editoroverlay_orientation_axis_shader_keeps_labels_screen_facing) {
    const std::string varying =
        ayt::render::detail::EditorOverlayPass::orientationAxisVaryingDef();
    const std::string vertex =
        ayt::render::detail::EditorOverlayPass::orientationAxisVertexShader();
    CHECK(varying.find("a_texcoord0") != std::string::npos);
    CHECK(vertex.find("clipPosition.xy += a_texcoord0 * clipPosition.w")
          != std::string::npos);
}

TEST_CASE(make_default_omits_editoroverlay) {
    const RenderPipelineDesc def = RenderPipelineDesc::makeDefault();
    CHECK(!def.contains(RenderPassSlot::EditorOverlay));
}

TEST_CASE(make_editor_forward_inserts_overlay_after_present) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeEditorForward();
    CHECK(desc.contains(RenderPassSlot::EditorOverlay));
    CHECK(desc.passes.size() == 14u);
    CHECK(desc.passes[2] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[3] == RenderPassSlot::Transparent);
    CHECK(desc.passes[4] == RenderPassSlot::Forward2DOpaque);
    CHECK(desc.passes[7] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[8] == RenderPassSlot::FXAA);
    CHECK(desc.passes[9] == RenderPassSlot::SMAA);
    CHECK(desc.passes[10] == RenderPassSlot::ColorGrading);
    CHECK(desc.passes[11] == RenderPassSlot::Present);
    CHECK(desc.passes[12] == RenderPassSlot::EditorOverlay);
    CHECK(desc.passes[13] == RenderPassSlot::UI);
}

TEST_CASE(make_editor_deferred_inserts_overlay_after_present) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeEditorDeferred();
    CHECK(desc.contains(RenderPassSlot::EditorOverlay));
    const auto ppIt = std::find(desc.passes.begin(), desc.passes.end(),
                                RenderPassSlot::PostProcess);
    CHECK(ppIt != desc.passes.end());
    CHECK(ppIt + 1 != desc.passes.end());
    CHECK(*(ppIt + 1) == RenderPassSlot::TAA);
    CHECK(ppIt + 2 != desc.passes.end());
    CHECK(*(ppIt + 2) == RenderPassSlot::FXAA);
    CHECK(ppIt + 3 != desc.passes.end());
    CHECK(*(ppIt + 3) == RenderPassSlot::SMAA);
    CHECK(ppIt + 4 != desc.passes.end());
    CHECK(*(ppIt + 4) == RenderPassSlot::ColorGrading);
    CHECK(ppIt + 5 != desc.passes.end());
    CHECK(*(ppIt + 5) == RenderPassSlot::Present);
    CHECK(ppIt + 6 != desc.passes.end());
    CHECK(*(ppIt + 6) == RenderPassSlot::EditorOverlay);
    CHECK(desc.contains(RenderPassSlot::GBufferDebug));
}

TEST_SUITE_END
