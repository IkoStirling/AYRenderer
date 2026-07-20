#pragma once

// P1 (2026-07-20, PR-C) — collapsed 12-argument RenderPass::execute
// signature. Bundles everything a pass needs to issue one frame's
// draws except for the things that are intrinsic to the pass itself
// (its name, its enable flag, its FBO / program caches).
//
// Why now:
//   - Adding new render-pass machinery (ShadowMap slot, GBuffer MRT,
//     offscreen scene RT for PostProcess closure, future DrawListBuilder
//     state) without this struct means every addition touches ALL
//     pass signatures AND every call site. We already saw the cost in
//     the cut-1 bisect (§5 of docs/execution-plan.md) when an extra
//     FrameContext field implied a 6-TU signature rewrite.
//   - The 12-arg call site in RenderPipeline::executeAll / Renderer::render
//     is fragile — argument swaps at the call site are silent because
//     every parameter is the same POD-ish type (`uint16_t viewportX` vs
//     `uint16_t viewportWidth`). A struct puts each field behind a
//     name.
//
// What stays OUT of the struct (intentional):
//   - The pass's own `name()` and `isEnabled()` state — those belong
//     to the RenderPass subclass, not the per-frame dispatch payload.
//   - Per-pass cached GPU resources (FBO handle, fullscreen VB/IB,
//     program handle). They're allocated once and live on the pass.
//   - Anything that would force `frame` to become non-const. Per
//     docs/execution-plan.md §5.3, the FrameContext reference stays
//     const until the §5.4 isolation experiments prove expanding it
//     is safe. This struct lets us add new fields (e.g. ShadowMap
//     slot, scene RT handle) without making `frame` mutable.
//
// `materials` is the only non-const map reference because Pass
// implementations lazily resolve BindingIds on first use and cache
// them in GpuMaterial::colorBinding / mat4Binding / boneBlockBinding
// (see RenderPass.cpp::resolveAndApplyColorUniforms). Pass-internal
// mutation, not frame-state mutation.

#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"
#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"

#include <cstdint>
#include <unordered_map>

namespace ayt::render::detail
{

struct PassExecContext {
    BGFXAdapter&            adapter;
    shader::ShaderResourcePool& pool;
    const RenderScene&      scene;
    const std::unordered_map<uint64_t, GpuMesh>&     meshes;
    const std::unordered_map<uint64_t, GpuTexture>&  textures;
    std::unordered_map<uint64_t, GpuMaterial>&       materials;

    // Viewport sub-rect this pass owns. UI passes typically take
    // the full window rect; scene passes take the 3D viewport.
    uint16_t                viewportX      = 0;
    uint16_t                viewportY      = 0;
    uint16_t                viewportWidth  = 0;
    uint16_t                viewportHeight = 0;

    // Frame data. Const by design (§5.3) — additions are append-only
    // POD fields, not state mutations.
    const FrameContext&     frame;

    // bgfx view id this pass should bind / submit into. Composite
    // mode hands 1 to the scene passes; non-composite hands 0. UIPass
    // ignores it (UIRenderBackend hard-codes kViewId = 2 today).
    uint8_t                 viewId         = 0;
};

} // namespace ayt::render::detail