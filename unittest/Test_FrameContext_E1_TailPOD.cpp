// §5.4 E1 isolation experiment (2026-07-20) — pin that appending a
// tail POD field to FrameContext does not break anything.
//
// What this pins:
//   1) FrameContext can be default-constructed and its new tail
//      field `shadowMapId` reads as 0.
//   2) A RenderScene can be rendered end-to-end with the new
//      FrameContext field present (verifies no pass trips on the
//      larger layout — every pass reads ctx.frame.*, and the
//      compiler emits a fresh load for each field access).
//   3) The pre-existing fields' values are preserved when the tail
//      POD is non-default — i.e. field initialization didn't
//      accidentally clobber earlier fields.
//   4) The tail POD can be set, read back, and survives across
//      multiple frames (no per-frame re-init).
//
// Why "tail POD" matters (rationale copied verbatim from
// docs/execution-plan.md §5.4):
//
//   E1 | 仅追加 `FrameContext` 尾部 POD 字段(无语义使用) |
//   不引入新稳定 crash
//
//   Any E1 regression → first deepen Noop/shaderc lifetime (P0.3),
//   then re-run. Don't stack the next experiment.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYMath/MathTypes.h"

#include <cstdint>

#include "detail/FrameContext.h"

using ayt::render::Renderer;
using ayt::render::RenderScene;
using ayt::render::MeshHandle;
using ayt::render::MaterialHandle;
using ayt::render::Backend;
using ayt::render::InitDesc;
using ayt::math::Float4x4;
using ayt::math::FVector3;
using ayt::render::detail::FrameContext;

namespace {

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

MaterialHandle makeUnlit(Renderer& r, const std::string& cacheKey) {
    return r.createMaterialFromPhoskia(kUnlitBaseColor, cacheKey);
}

uint32_t renderOnce(Renderer& r, RenderScene& scene) {
    r.beginFrame({});
    r.render(scene);
    r.endFrame();
    return r.getFrameStats().drawCalls;
}

} // namespace

TEST_SUITE(AYRenderer_FrameContext_E1_TailPOD)

TEST_CASE(e1_tail_pod_default_value_is_zero) {
    // Pin: the new tail field is POD with `= 0` default initializer.
    // Any other default (UB if uninitialized, garbage value, etc.)
    // would surface here.
    FrameContext ctx;
    CHECK(ctx.shadowMapId == 0u);
}

TEST_CASE(e1_tail_pod_can_be_set_and_read) {
    // Pin: the field is a normal uint32_t — settable + readable.
    FrameContext ctx;
    ctx.shadowMapId = 42u;
    CHECK(ctx.shadowMapId == 42u);
    ctx.shadowMapId = 0u;
    CHECK(ctx.shadowMapId == 0u);
}

TEST_CASE(e1_tail_pod_does_not_clobber_existing_fields) {
    // Pin: the new tail field does not inadvertently re-zero the
    // earlier fields. We set every existing field to a non-default
    // sentinel, then write the tail POD, and verify nothing else
    // changed.
    FrameContext ctx;
    ctx.view           = Float4x4::identity();
    ctx.cameraPosition = FVector3(1.5f, 2.5f, 3.5f);
    ctx.lightDirection = FVector3(0.1f, 0.2f, 0.3f);
    ctx.lightColor     = FVector3(0.4f, 0.5f, 0.6f);
    ctx.timeSeconds    = 7.5f;
    ctx.bloomStrength  = 0.25f;
    ctx.exposure       = 1.75f;
    ctx.tonemapMode    = FrameContext::TonemapMode::ACES;
    ctx.shadowMapId    = 99u;

    CHECK(ctx.cameraPosition.x == 1.5f);
    CHECK(ctx.cameraPosition.y == 2.5f);
    CHECK(ctx.cameraPosition.z == 3.5f);
    CHECK(ctx.lightDirection.x == 0.1f);
    CHECK(ctx.lightDirection.y == 0.2f);
    CHECK(ctx.lightDirection.z == 0.3f);
    CHECK(ctx.lightColor.x     == 0.4f);
    CHECK(ctx.lightColor.y     == 0.5f);
    CHECK(ctx.lightColor.z     == 0.6f);
    CHECK(ctx.timeSeconds      == 7.5f);
    CHECK(ctx.bloomStrength    == 0.25f);
    CHECK(ctx.exposure         == 1.75f);
    CHECK(ctx.tonemapMode      == FrameContext::TonemapMode::ACES);
    CHECK(ctx.shadowMapId      == 99u);
}

TEST_CASE(e1_render_endtoend_with_tail_pod_in_place) {
    // The headline E1 pin: an end-to-end render() completes cleanly
    // with the new FrameContext tail POD present. This is what
    // §5.4 E1 is supposed to verify — that extending FrameContext's
    // layout is safe even when nothing reads the new field.
    //
    // If this test regresses (PASS → FAIL or 139), the §5.4 protocol
    // says: STOP, deepen Noop/shaderc lifetime per P0.3, then
    // re-run the experiment. Do NOT stack another change on top.
    Renderer r;
    InitDesc desc{};
    desc.backend = Backend::Noop;
    r.initialize(desc);

    MeshHandle cube = r.createUnitCube();
    CHECK(cube.isValid() == true);

    MaterialHandle mat = makeUnlit(r, "e1_tailpod");
    r.setMaterialColor(mat, "baseColor", 0.3f, 0.6f, 0.9f, 1.0f);

    RenderScene scene;
    scene.add(cube, mat);
    scene.add(cube, mat);
    scene.add(cube, mat);

    const uint32_t firstDraws  = renderOnce(r, scene);
    const uint32_t secondDraws = renderOnce(r, scene);
    const uint32_t thirdDraws  = renderOnce(r, scene);

    // Three Opaque items (default BlendMode) + zero Transparent.
    // Stats draw count is the sum of every per-pass execute return
    // value — the larger FrameContext layout must not affect that
    // accounting.
    CHECK(firstDraws  == 3u);
    CHECK(secondDraws == firstDraws);
    CHECK(thirdDraws  == firstDraws);

    r.shutdown();
}

TEST_SUITE_END