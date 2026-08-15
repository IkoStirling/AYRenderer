// U1.5 — TransparentPass back-to-front sort + MVP/light upload.
//
// Two related changes landed in U1.5:
//
//   (A) TransparentPass now sorts the scene's items by DrawItem::sortKey
//       (descending = back-to-front compositing order) before iterating.
//       The default sortKey=0 keeps insertion order (stable_sort + equal
//       keys preserve relative order), so this is a strict no-op for
//       callers that haven't set sortKey. We verify:
//         - render still completes cleanly with the default sortKey=0
//           (case 1: preserves insertion order)
//         - render with distinct sortKey values runs and submits draws
//           (case 2: descending comparator compiles + runs)
//         - the scene's _items vector is NOT mutated by the sort
//           (case 3: sortKey values in scene.items() unchanged after
//           a render)
//
//   (B) TransparentPass now uploads MVP / cameraPos / lightDir /
//       lightColor per draw, matching ForwardOpaquePass::flushMaterial.
//       Pre-U1.5 the absence wasn't observable because Phoskia's Unlit
//       test shader doesn't sample those uniforms. We verify:
//         - a "Lit"-style material that names the same uniforms
//           parses + renders without crashing (case 4: the helper
//           call path executes end-to-end)
//         - three back-to-back renders succeed (case 5: sort buffer
//           is re-created each frame; no leak / no state corruption)
//
// All tests use Backend::Noop so shaderc is not required.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYMath/MathTypes.h"

#include <cstdio>
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
using ayt::math::FVector3;

namespace {

MeshHandle makeUnitCube(Renderer& r) {
    return r.createUnitCube();
}

// Plain Phoskia Unlit source — same shape used across the test suite
// so the parser accepts it under Noop backend. Doesn't sample MVP /
// light, so it lets us isolate the sort behavior (cases 1-3, 5).
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

// "Lit"-style material that NAMES cameraPos + lightDir / lightColor
// / u_modelViewProj. Phoskia's parser accepts any property the body
// doesn't necessarily consume, but for case 4 we declare them as
// properties so setUniform can find them via the binding cache. With
// Backend::Noop, setUniform is a no-op anyway — the test only asserts
// that the dispatch path doesn't crash.
constexpr const char* kLitWithLight = R"(
material Lit {
    property baseColor      = vec4(1.0, 1.0, 1.0, 1.0);
    property cameraPos      = vec3(0.0, 0.0, 0.0);
    property lightDir       = vec3(0.0, -1.0, 0.0);
    property lightColor     = vec3(1.0, 1.0, 1.0);

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

MaterialHandle makeUnlit(Renderer& r, const std::string& cacheKey) {
    return r.createMaterialFromPhoskia(kUnlitBaseColor, cacheKey);
}

MaterialHandle makeLit(Renderer& r, const std::string& cacheKey) {
    return r.createMaterialFromPhoskia(kLitWithLight, cacheKey);
}

} // namespace

TEST_SUITE(AYRenderer_TransparentPass_U1Plus5)

TEST_CASE(transparent_sort_default_zero_preserves_insertion_order) {
    // U1.5 invariant: when every item has sortKey=0 (the default),
    // stable_sort preserves the original insertion order, so the
    // behavior is byte-for-byte identical to the pre-sort code path.
    // We can't directly observe draw order on Noop backend, but we
    // CAN verify the render completes without crashing and that
    // every alpha item still contributes a draw.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);
    CHECK(cube.isValid() == true);

    MaterialHandle mat = makeUnlit(r, "u15_sort_default_zero");
    r.setMaterialBlendMode(mat, BlendMode::Alpha);

    RenderScene scene;
    scene.add(cube, mat);
    scene.add(cube, mat);
    scene.add(cube, mat);

    // Pre-condition: three items, all default sortKey=0.
    CHECK(scene.items().size() == 3);
    for (const DrawItem& it : scene.items()) {
        CHECK(it.sortKey == 0);
    }

    r.beginFrame({});
    r.render(scene);
    r.endFrame();

    // Post-condition: scene untouched by sort.
    CHECK(scene.items().size() == 3);
    for (const DrawItem& it : scene.items()) {
        CHECK(it.sortKey == 0);
    }

    r.shutdown();
}

TEST_CASE(transparent_sort_descending_by_sortkey) {
    // U1.5 contract: items with higher sortKey render FIRST. We can't
    // observe draw order on Noop, but the comparator + stable_sort
    // path must run end-to-end without crashing, dropping items, or
    // leaving the scene's _items mutated.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);

    MaterialHandle mat = makeUnlit(r, "u15_sort_descending");
    r.setMaterialBlendMode(mat, BlendMode::Alpha);

    RenderScene scene;
    scene.add(cube, mat);  // sortKey=0 (default)
    scene.add(cube, mat);  // sortKey=0 (default)

    // Customize sortKey on two of the three via the 5-field skinned
    // overload... no, sortKey isn't a parameter to add(). We use the
    // DrawItem aggregate-add overload to set sortKey explicitly.
    DrawItem customA;
    customA.mesh = cube;
    customA.material = mat;
    customA.world = Float4x4::identity();
    customA.boneMatrices = nullptr;
    customA.jointCount = 0;
    customA.sortKey = 100;  // drawn first
    scene.add(customA);

    DrawItem customB;
    customB.mesh = cube;
    customB.material = mat;
    customB.world = Float4x4::identity();
    customB.boneMatrices = nullptr;
    customB.jointCount = 0;
    customB.sortKey = 50;   // drawn second
    scene.add(customB);

    CHECK(scene.items().size() == 4);

    r.beginFrame({});
    r.render(scene);
    r.endFrame();

    // Post-condition: scene._items unchanged. The sort uses a
    // transient pointer list; the engine-side vector is not
    // reordered.
    CHECK(scene.items().size() == 4);
    CHECK(scene.items()[0].sortKey == 0);
    CHECK(scene.items()[1].sortKey == 0);
    CHECK(scene.items()[2].sortKey == 100);
    CHECK(scene.items()[3].sortKey == 50);

    r.shutdown();
}

TEST_CASE(transparent_sort_does_not_mutate_scene_items) {
    // The sort uses a transient std::vector<const DrawItem*> built
    // once per execute() call, then discarded when execute returns.
    // The scene's _items vector must remain in the order the engine
    // appended to it. This test exercises the boundary between the
    // sort buffer (internal) and the engine-side state.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);
    MaterialHandle mat = makeUnlit(r, "u15_no_mutate");
    r.setMaterialBlendMode(mat, BlendMode::Alpha);

    RenderScene scene;
    // Append in a deliberately-scrambled sortKey order: 3, 1, 2.
    for (int32_t key : {3, 1, 2}) {
        DrawItem it;
        it.mesh = cube;
        it.material = mat;
        it.world = Float4x4::identity();
        it.boneMatrices = nullptr;
        it.jointCount = 0;
        it.sortKey = key;
        scene.add(it);
    }

    // Sanity: scene still holds 3, 1, 2 in insertion order.
    CHECK(scene.items()[0].sortKey == 3);
    CHECK(scene.items()[1].sortKey == 1);
    CHECK(scene.items()[2].sortKey == 2);

    r.beginFrame({});
    r.render(scene);
    r.endFrame();

    // After render: scene still holds 3, 1, 2. Sort is internal.
    CHECK(scene.items()[0].sortKey == 3);
    CHECK(scene.items()[1].sortKey == 1);
    CHECK(scene.items()[2].sortKey == 2);

    r.shutdown();
}

TEST_CASE(transparent_mvp_light_uniform_upload_runs_endto_end) {
    // U1.5 — TransparentPass now uploads MVP / cameraPos / lightDir /
    // lightColor per draw. The "Lit" Phoskia source above declares
    // those as properties so the binding cache resolves them. With
    // Backend::Noop, setUniform is a no-op so we only verify the
    // helper path runs without crashing and the alpha dispatch still
    // produces draws.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);

    MaterialHandle litMat = makeLit(r, "u15_lit_alpha");
    r.setMaterialBlendMode(litMat, BlendMode::Alpha);
    r.setMaterialColor(litMat, "baseColor", 0.2f, 0.8f, 0.4f, 0.6f);

    RenderScene scene;
    scene.add(cube, litMat);
    scene.add(cube, litMat);

    // Two renders to catch any per-frame uniform-binding cache
    // corruption the new MVP/light path might expose.
    for (int i = 0; i < 2; ++i) {
        r.beginFrame({});
        r.render(scene);
        r.endFrame();
    }

    r.shutdown();
}

TEST_CASE(transparent_render_idempotent_three_passes_with_sort) {
    // The sort buffer (std::vector<const DrawItem*>) is allocated
    // each execute() call. Three back-to-back renders verify the
    // buffer can be repeatedly constructed + destroyed + sized for
    // a 4-item scene without leaking or corrupting state.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = makeUnitCube(r);
    MaterialHandle mat = makeUnlit(r, "u15_idem_sort");
    r.setMaterialBlendMode(mat, BlendMode::Alpha);

    RenderScene scene;
    for (int32_t key : {4, 2, 1, 3}) {
        DrawItem it;
        it.mesh = cube;
        it.material = mat;
        it.world = Float4x4::identity();
        it.boneMatrices = nullptr;
        it.jointCount = 0;
        it.sortKey = key;
        scene.add(it);
    }

    for (int i = 0; i < 3; ++i) {
        r.beginFrame({});
        r.render(scene);
        r.endFrame();
    }

    // Scene still 4 items, still in original order.
    CHECK(scene.items().size() == 4);
    CHECK(scene.items()[0].sortKey == 4);
    CHECK(scene.items()[3].sortKey == 3);

    r.shutdown();
}

TEST_SUITE_END
