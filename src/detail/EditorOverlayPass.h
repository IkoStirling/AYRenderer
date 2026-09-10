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
//   view 252 — selected-object transform gizmo (axis arrows, plane handles,
//             rotation rings and scale handles), drawn procedurally.
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
    static constexpr uint8_t kGizmoViewId = kBlitViewId;
    static constexpr uint8_t kGridViewId  = kGizmoViewId;

    ~EditorOverlayPass() override = default;

    std::string_view name() const override { return "EditorOverlay"; }

    uint32_t execute(PassExecContext& ctx) override;

    void setOrientationAxisEnabled(bool enabled) noexcept {
        _orientationAxisEnabled = enabled;
    }
    bool orientationAxisEnabled() const noexcept {
        return _orientationAxisEnabled;
    }
    void setTransformGizmoState(
        const EditorTransformGizmoState& state) noexcept {
        _transformGizmo = state;
    }
    const EditorTransformGizmoState& transformGizmoState() const noexcept {
        return _transformGizmo;
    }
    void setGrid2DState(const EditorGrid2DState& state) noexcept {
        _grid2D = state;
    }
    const EditorGrid2DState& grid2DState() const noexcept { return _grid2D; }
    void setSelectionOutline2DState(
        const EditorSelectionOutline2DState& state) noexcept {
        _selectionOutline2D = state;
    }
    const EditorSelectionOutline2DState& selectionOutline2DState() const noexcept {
        return _selectionOutline2D;
    }
    void setUnjitteredProjection(
        const ayt::math::Float4x4& projection) noexcept {
        _unjitteredProjection = projection;
        _hasUnjitteredProjection = true;
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
    uint32_t submitTransformGizmo(PassExecContext& ctx,
                                  const FrameContext& frame);
    bool ensureTransformGizmoResources(PassExecContext& ctx);
    void destroyTransformGizmoGeometry(BGFXAdapter& adapter);
    uint32_t submitGrid2D(PassExecContext& ctx, const FrameContext& frame);
    bool ensureGrid2DResources(PassExecContext& ctx);
    void destroyGrid2DGeometry(BGFXAdapter& adapter);
    uint32_t submitSelectionOutline2D(PassExecContext& ctx,
                                      const FrameContext& frame);
    bool ensureSelectionOutline2DResources(PassExecContext& ctx);
    void destroySelectionOutline2DGeometry(BGFXAdapter& adapter);

    bool _orientationAxisEnabled = false;
    bool _axisProgramAcquireFailed = false;
    bgfx::VertexBufferHandle _axisVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  _axisIndexBuffer  = BGFX_INVALID_HANDLE;
    uint32_t _axisArrowIndexCount = 0;
    uint32_t _axisLabelIndexStart = 0;
    uint32_t _axisLabelIndexCount = 0;
    ayt::shader::ShaderResource _axisProgram;

    EditorTransformGizmoState _transformGizmo{};
    EditorTransformGizmoMode _builtGizmoMode =
        EditorTransformGizmoMode::Hidden;
    uint8_t _builtGizmoHighlight = 0xffu;
    uint16_t _builtGizmoDisabledHandleMask = 0xffffu;
    bgfx::VertexBufferHandle _gizmoVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle _gizmoIndexBuffer = BGFX_INVALID_HANDLE;
    uint32_t _gizmoPrimaryIndexCount = 0;
    uint32_t _gizmoIndexCount = 0;
    ayt::math::Float4x4 _unjitteredProjection =
        ayt::math::Float4x4::identity();
    bool _hasUnjitteredProjection = false;

    EditorGrid2DState _grid2D{};
    EditorGrid2DState _builtGrid2D{};
    uint16_t _builtGridViewportWidth = 0;
    uint16_t _builtGridViewportHeight = 0;
    bgfx::VertexBufferHandle _gridVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle _gridIndexBuffer = BGFX_INVALID_HANDLE;
    uint32_t _gridIndexCount = 0;

    EditorSelectionOutline2DState _selectionOutline2D{};
    EditorSelectionOutline2DState _builtSelectionOutline2D{};
    bgfx::VertexBufferHandle _selectionOutline2DVertexBuffer =
        BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle _selectionOutline2DIndexBuffer =
        BGFX_INVALID_HANDLE;
    uint32_t _selectionOutline2DIndexCount = 0;
};

} // namespace ayt::render::detail
