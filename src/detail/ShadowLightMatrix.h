#pragma once

// PR-F1' (safe cut, 2026-07-20) — directional light-space matrices for
// ShadowPass. Lives OUTSIDE FrameContext / RenderScene on purpose:
// the blocked C' F1 (Light struct + FrameContext shadowFbo/lightViewProj
// + default-enabled Shadow) SIGSEGV'd; see docs/execution-plan.md §5.5.
//
// Matrices use bx (same convention as Renderer::setMainCameraLookAtPerspective)
// so D3D11 homogeneousDepth matches the rest of the pipeline. AYMath
// lookAt/ortho are intentionally NOT used here.

#include "AYMath/MathTypes.h"

namespace ayt::render::detail
{

// Fixed-radius orthographic frustum centered at `focus`, looking along
// `lightDirection` (travel direction, same as setDirectionalLight).
// `homogeneousDepth` must match bgfx::getCaps()->homogeneousDepth when
// the adapter is live; unit tests may pass false.
void buildDirectionalShadowMatrices(
    const ayt::math::FVector3& lightDirection,
    ayt::math::Float4x4& outView,
    ayt::math::Float4x4& outProj,
    ayt::math::Float4x4& outViewProj,
    float outViewColMajor[16],
    float outProjColMajor[16],
    float outViewProjColMajor[16],
    ayt::math::FVector3 focus = ayt::math::FVector3(0.0f, 0.0f, 0.0f),
    float radius = 50.0f,
    bool homogeneousDepth = false);

} // namespace ayt::render::detail
