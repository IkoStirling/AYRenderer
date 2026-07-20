// P0.4 (2026-07-20) — ForwardOpaquePass now skips BlendMode::Alpha
// items so they aren't double-submitted (was: ForwardOpaque + Transparent
// both drew them — alpha pixels written to depth buffer, z-blocking
// legit back-to-front compositing + wasting GPU).
//
// We pin the new contract by observing RenderFrameStats.drawCalls
// across two scenes with identical item counts but different
// Opaque:Alpha ratios. The single public counter the renderer
// exposes is the SUM of every per-pass execute() return value
// (Renderer::Impl::lastDrawCalls → DebugOverlay::stats().drawCalls).
// On Backend::Noop, bgfx::submit is a no-op but `++drawCount` in
// each pass still runs (the pass returns its local counter before
// any bgfx submission that depends on a live backend), so the
// sum is observable.
//
// Test cases:
//   1) opaque_alpha_split_counts
//      — 1 Opaque + 3 Alpha items. Before P0.4 the sum was
//        ForwardOpaque(4) + Transparent(3) = 7. After P0.4 the sum
//        is ForwardOpaque(1) + Transparent(3) = 4. We assert == 4.
//   2) opaque_only_unchanged
//      — 4 Opaque items. P0.4 must NOT regress Opaque counts:
//        ForwardOpaque(4) + Transparent(0) = 4.
//   3) alpha_only_transparent_only
//      — 3 Alpha items. ForwardOpaque(0) + Transparent(3) = 3.
//        Catches the off-by-one where the skip accidentally skips
//        all materials.
//   4) mix_three_alpha_two_opaque
//      — 2 Opaque + 3 Alpha. ForwardOpaque(2) + Transparent(3) = 5.
//   5) alpha_skip_is_idempotent_across_frames
//      — Render the same scene 3 times. stats.drawCalls is identical
//        each frame (no per-frame regression in the skip path's
//        blendMode lookup — cheap field read, but pin anyway).

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "aymath/MathTypes.h"

#include <string>

using ayt::render::Renderer;
using ayt::render::RenderScene;
using ayt::render::DrawItem;
using ayt::render::MeshHandle;
using ayt::render::MaterialHandle;
using ayt::render::Backend;
using ayt::render::BlendMode;
using ayt::render::InitDesc;
using ayt::math::Float4x4;

namespace {

// Minimal Phoskia Unlit source — same shape used across the test suite
// so the parser accepts it under Noop backend (Phoskia syntax is
// fragile; we keep the exact source shape).
constexpr const char* kUnlitBaseColor = R"(
material Unlit {
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0);

    vertex {
        in  position : position;
        out position : position;
        return vec4(position, 1.0);
    }
    fragment {
        in  position : position;
        return baseColor;
    }
}
)";

MeshHandle makeUnitCube(Renderer& r) {
    return r.createUnitCube();
}

MaterialHandle makeUnlit(Renderer& r, const std::string& cacheKey) {
    return r.createMaterialFromPhoskia(kUnlitBaseColor, cacheKey);
}

// Render a scene and return stats().drawCalls from the FRAME just
// completed. We must endFrame() before reading stats because
// DebugOverlay only updates _stats.drawCalls in onEndFrame.
uint32_t renderOnce(Renderer& r, RenderScene& scene) {
    r.beginFrame({});
    r.render(scene);
    r.endFrame();
    return r.getFrameStats().drawCalls;
}

} // namespace

TEST_SUITE(AYRenderer_ForwardOpaque_BlendSkip_P0_4)

TEST_CASE(opaque_alpha_split_counts) {
    // The headline behavior pin: Opaque+Alpha scene total draw count
    // drops by exactly the number of Alpha items after P0.4.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);
    CHECK(cube.isValid() == true);

    MaterialHandle opaqueMat = makeUnlit(r, "p04_opaque");
    CHECK(opaqueMat.isValid() == true);
    r.setMaterialColor(opaqueMat, "baseColor", 0.2f, 0.6f, 1.0f, 1.0f);

    MaterialHandle alphaMat = makeUnlit(r, "p04_alpha");
    CHECK(alphaMat.isValid() == true);
    r.setMaterialBlendMode(alphaMat, BlendMode::Alpha);

    RenderScene scene;
    scene.add(cube, opaqueMat);   // Opaque — should hit ForwardOpaque
    scene.add(cube, alphaMat);    // Alpha  — should ONLY hit Transparent
    scene.add(cube, alphaMat);    // Alpha  — should ONLY hit Transparent
    scene.add(cube, alphaMat);    // Alpha  — should ONLY hit Transparent

    // Expected after P0.4:
    //   ForwardOpaque draws the 1 Opaque item      → 1
    //   Transparent draws the 3 Alpha items        → 3
    //   Total                                       → 4
    // Pre-P0.4 (regression baseline) would be 4 + 3 = 7.
    const uint32_t draws = renderOnce(r, scene);
    CHECK(draws == 4u);

    r.shutdown();
}

TEST_CASE(opaque_only_unchanged) {
    // 4 Opaque items. P0.4 must NOT regress Opaque counts.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);
    MaterialHandle opaqueMat = makeUnlit(r, "p04_opaque_only");
    r.setMaterialColor(opaqueMat, "baseColor", 0.4f, 0.4f, 0.4f, 1.0f);

    RenderScene scene;
    for (int i = 0; i < 4; ++i) {
        scene.add(cube, opaqueMat);
    }

    // 4 Opaque items: ForwardOpaque(4) + Transparent(0) = 4.
    const uint32_t draws = renderOnce(r, scene);
    CHECK(draws == 4u);

    r.shutdown();
}

TEST_CASE(alpha_only_transparent_only) {
    // 3 Alpha items. ForwardOpaque skips them all; Transparent draws all 3.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);
    MaterialHandle alphaMat = makeUnlit(r, "p04_alpha_only");
    r.setMaterialBlendMode(alphaMat, BlendMode::Alpha);

    RenderScene scene;
    for (int i = 0; i < 3; ++i) {
        scene.add(cube, alphaMat);
    }

    // ForwardOpaque(0) + Transparent(3) = 3.
    const uint32_t draws = renderOnce(r, scene);
    CHECK(draws == 3u);

    r.shutdown();
}

TEST_CASE(mix_two_opaque_three_alpha) {
    // 2 Opaque + 3 Alpha. Expected: 2 + 3 = 5.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);
    MaterialHandle opaqueMat = makeUnlit(r, "p04_mix_op");
    r.setMaterialColor(opaqueMat, "baseColor", 0.8f, 0.1f, 0.1f, 1.0f);
    MaterialHandle alphaMat = makeUnlit(r, "p04_mix_al");
    r.setMaterialBlendMode(alphaMat, BlendMode::Alpha);

    RenderScene scene;
    scene.add(cube, opaqueMat);
    scene.add(cube, opaqueMat);
    scene.add(cube, alphaMat);
    scene.add(cube, alphaMat);
    scene.add(cube, alphaMat);

    const uint32_t draws = renderOnce(r, scene);
    CHECK(draws == 5u);

    r.shutdown();
}

TEST_CASE(alpha_skip_is_idempotent_across_frames) {
    // Three back-to-back renders of the same scene. stats.drawCalls
    // must be identical each frame — pins the skip's cheap blendMode
    // field read against any per-frame state corruption.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);
    MaterialHandle opaqueMat = makeUnlit(r, "p04_idem_op");
    r.setMaterialColor(opaqueMat, "baseColor", 1.0f, 1.0f, 1.0f, 1.0f);
    MaterialHandle alphaMat = makeUnlit(r, "p04_idem_al");
    r.setMaterialBlendMode(alphaMat, BlendMode::Alpha);

    RenderScene scene;
    scene.add(cube, opaqueMat);
    scene.add(cube, alphaMat);
    scene.add(cube, alphaMat);

    const uint32_t firstDraws  = renderOnce(r, scene);
    const uint32_t secondDraws = renderOnce(r, scene);
    const uint32_t thirdDraws  = renderOnce(r, scene);

    CHECK(firstDraws  == 3u);
    CHECK(secondDraws == firstDraws);
    CHECK(thirdDraws  == firstDraws);

    r.shutdown();
}

TEST_SUITE_END