#pragma once

#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"

#include "AYShader/ShaderResource.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Editor overlay AFTER Present, BEFORE UI. Selection hulls intentionally do
// not live here: Present has already detached scene depth, so a mesh redraw in
// this pass becomes a full-object overlay. TransparentPass owns the depth-aware
// selection submissions while scene color + depth are still paired.
//
//   view 251 — procedural camera-orientation axis, muted negative-axis tails,
//             and screen-facing X/Y/Z labels in the viewport's lower-left
//             corner (no texture; rotation-only camera view).
//   view 252 — reserved compatibility id; selection no longer draws here.
//
// 17 belongs to FXAA and 18..25 belong to the shadow atlas.
// 26..249 belong to UI offscreen layers, 250 to GBufferDebug, and 255 to UI.
class EditorOverlayPass : public RenderPass {
public:
    static constexpr uint8_t kAxisViewId  = 251;
    // Compatibility alias retained for existing diagnostics/tests that knew
    // the former reserved-mask name.
    static constexpr uint8_t kMaskViewId  = kAxisViewId;
    static constexpr uint8_t kBlitViewId  = 252;

    ~EditorOverlayPass() override = default;

    std::string_view name() const override { return "EditorOverlay"; }

    uint32_t execute(PassExecContext& ctx) override;

    void setOrientationAxisEnabled(bool enabled) noexcept {
        _orientationAxisEnabled = enabled;
    }
    bool orientationAxisEnabled() const noexcept {
        return _orientationAxisEnabled;
    }

    // Exposed as pure math for contract tests. Scene-camera translation is
    // intentionally removed; the widget lives at +3 in its private view.
    static ayt::math::Float4x4 makeOrientationAxisView(
        const ayt::math::Float4x4& sceneView);
    static const char* orientationAxisVaryingDef() noexcept;
    static const char* orientationAxisVertexShader() noexcept;
    static const char* orientationAxisFragmentShader() noexcept;

    // Must run while both adapter and shader pool are alive. Idempotent.
    void destroyResources(BGFXAdapter& adapter);

private:
    uint32_t submitOrientationAxis(PassExecContext& ctx,
                                   const FrameContext& frame);
    bool ensureOrientationAxisResources(PassExecContext& ctx);

    bool _orientationAxisEnabled = false;
    bool _axisProgramAcquireFailed = false;
    bgfx::VertexBufferHandle _axisVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  _axisIndexBuffer  = BGFX_INVALID_HANDLE;
    uint32_t _axisArrowIndexCount = 0;
    uint32_t _axisLabelIndexStart = 0;
    uint32_t _axisLabelIndexCount = 0;
    ayt::shader::ShaderResource _axisProgram;
};

} // namespace ayt::render::detail
