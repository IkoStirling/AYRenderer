// §5.4 (2026-07-22) — ForwardOpaquePass + TransparentPass
// isInitialized() guard verification.
//
// Background: §5.4 commits a `isInitialized()` early-exit at the
// top of every Pass::execute() so an uninitialized adapter cannot
// reach `adapter.setViewTransform` (raw bgfx::setViewTransform call
// — dereferences internal bgfx state, UB on uninit on most
// backends).
//
// B4a/B4b/B4c/B5 all added this gate cleanly. FO/Trans were the
// pre-existing landmine: they went straight into
// `adapter.setViewTransform(viewId, frame.view, frame.projection)`
// at ForwardOpaquePass.cpp:148 and TransparentPass.cpp:41 without
// any guard.
//
// IMPORTANT: the guard is `isInitialized()` ONLY, not
// `isInitialized() || isNoopBackend()`. The Noop backend
// short-circuits INSIDE BGFXAdapter (each draw command is gated
// there) — the Pass-level gate would skip the scene-items loop,
// which would break 7+ pre-existing tests that count logical draw
// submissions via `++drawCount` regardless of GPU outcome
// (`debug_overlay_reports_draw_count`,
// `forward_opaque_draw_one_frame`, `opaque_alpha_split_counts`,
// `capture_screenshot_*`, etc.). Those tests were implicitly
// relying on Noop NOT short-circuiting at the Pass level. The
// `isInitialized()` check alone fixes the actual UB without
// disturbing the test semantics.
//
// 2 cases:
//
//   1. ForwardOpaquePass::execute on an uninitialized adapter:
//      MUST return 0 draws without crashing. The adapter never
//      gets `bgfx::init` called, so any `bgfx::setViewTransform`
//      dereference is UB. Pre-fix, this either crashed or returned
//      undefined draws.
//
//   2. TransparentPass::execute on an uninitialized adapter: same
//      contract as case 1, different pass.

#include "AYTest.h"
#include "AYRenderScene.h"
#include "AYShaderResourcePool.h"

#include "AYMath/MathTypes.h"

#include "detail/BGFXAdapter.h"
#include "detail/FrameContext.h"
#include "detail/ForwardOpaquePass.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/TransparentPass.h"

#include <unordered_map>

using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::FrameContext;
using ayt::render::detail::ForwardOpaquePass;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;
using ayt::render::detail::TransparentPass;

TEST_SUITE(AYRenderer_FO_Trans_NoopGate)

TEST_CASE(fo_uninit_adapter_returns_zero) {
    // §5.4 (2026-07-22) — ForwardOpaquePass::execute must early-
    // exit on an uninitialized adapter BEFORE reaching
    // `adapter.setViewTransform(viewId, frame.view, frame.projection)`.
    //
    // Mirror GBufferPass.cpp:127, LightingPass.cpp:24, ShadowPass
    // .cpp:24-26: `isInitialized()` short-circuits to `return 0`
    // with zero GPU side effects. The `isNoopBackend()` check is
    // NOT part of this gate (see file-level comment for why).
    BGFXAdapter adapter;
    CHECK(adapter.isInitialized() == false);  // sanity — default-ctor = uninit

    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };

    ForwardOpaquePass pass;
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0u);
    CHECK(pass.name() == "ForwardOpaque");
}

TEST_CASE(transparent_uninit_adapter_returns_zero) {
    // §5.4 (2026-07-22) — TransparentPass::execute same gate fix as
    // FO. Same comment block as the FO case above — see file-level
    // rationale for why Noop is NOT gated at the Pass level.
    BGFXAdapter adapter;
    CHECK(adapter.isInitialized() == false);

    ayt::shader::ShaderResourcePool pool;
    RenderScene scene;
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    FrameContext frame;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };

    TransparentPass pass;
    const uint32_t draws = pass.execute(ctx);
    CHECK(draws == 0u);
    CHECK(pass.name() == "Transparent");
}

TEST_SUITE_END