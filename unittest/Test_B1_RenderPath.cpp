// PR-B1 (2026-07-22) — §P5 B1 plumbing smoke tests.
//
// Background (§P5 in docs/execution-plan.md + docs/deferred-pass.md):
//
//   B1 is the "RenderPath enum + RenderPipelineDesc::path" plumbing
//   step. Today (B1) Deferred is just a tagged clone of the Forward
//   pipeline — the actual branch (GBuffer + Lighting slots, view 7/8,
//   ForwardOpaque skip) lands in B3. Pre-wiring `path` now lets
//   Editor / hosts opt in via `configurePipeline(makeDeferred())`
//   without waiting on B3, and lets tests pin the enum semantics
//   today.
//
// What this suite pins:
//
//   1. Default-constructed RenderPipelineDesc carries path=Forward.
//   2. makeDefault() returns path=Forward.
//   3. makeForwardWithShadows() returns path=Forward (alias for
//      makeDefault per E5).
//   4. makeDeferred() returns path=Deferred BUT same 5-slot pass list
//      as Forward (the B1 stub contract — actual Deferred slots land
//      in B3). isDeferred() is the accessor.
//   5. configurePipeline(makeDeferred()) on a Renderer records the
//      path; subsequent pipelineDesc().isDeferred() reads true.
//   6. configurePipeline({}) empty desc still falls back to Forward
//      (the E5 behavior — empty desc means "use default", not "use
//      Deferred").
//   7. ABI sanity: RenderPipelineDesc stays trivially copyable / no
//      bgfx types leaked into the public header (the public header
//      surface gate is in Test_PublicHeaderSurface.cpp; this case
//      just confirms sizeof doesn't unexpectedly bloat).
//
// What this suite does NOT pin (deferred per cutsheet):
//
//   - view-id 7/8 allocation — lands in B4 (GBuffer MRT) and B5
//     (Lighting).
//   - Path-aware dispatch (ForwardOpaque skip under Deferred) — B3.
//   - PostProcess source-FBO selection per path — B6.
//   - Deferred ABI surface (GBufferPass / LightingPass public types) —
//     B2 / B3.

#include "AYRenderer.h"
#include "AYRenderTypes.h"
#include "AYTest.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

using ayt::render::Renderer;
using ayt::render::RenderPath;
using ayt::render::RenderPipelineDesc;
using ayt::render::RenderPassSlot;

TEST_SUITE(AYRenderer_B1_RenderPath)

TEST_CASE(b1_default_constructed_desc_is_forward)
{
    // B1.1 — Default ctor of RenderPipelineDesc yields path=Forward.
    // Default Forward is the load-bearing assumption: hosts that
    // never call configurePipeline() still get the E5 Forward
    // pipeline.
    RenderPipelineDesc desc;
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.isDeferred() == false);
    CHECK(desc.passes.empty());  // E5 contract: empty passes ⇒ fallback
}

TEST_CASE(b1_make_default_path_is_forward)
{
    // B1.2 — makeDefault() carries the Forward tag explicitly. The
    // default ctor path == makeDefault().path == RenderPath::Forward.
    const RenderPipelineDesc def = RenderPipelineDesc::makeDefault();
    CHECK(def.path == RenderPath::Forward);
    CHECK(def.isDeferred() == false);
    CHECK(def.passes.size() == 5u);
    CHECK(def.contains(RenderPassSlot::Shadow));
    CHECK(def.contains(RenderPassSlot::ForwardOpaque));
    CHECK(def.contains(RenderPassSlot::Transparent));
    CHECK(def.contains(RenderPassSlot::PostProcess));
    CHECK(def.contains(RenderPassSlot::UI));
}

TEST_CASE(b1_make_forward_with_shadows_path_is_forward)
{
    // B1.3 — makeForwardWithShadows() == makeDefault() per E5. Path
    // stays Forward (Shadow-on is orthogonal to path choice).
    const RenderPipelineDesc fws = RenderPipelineDesc::makeForwardWithShadows();
    CHECK(fws.path == RenderPath::Forward);
    CHECK(fws.isDeferred() == false);
    CHECK(fws.passes.size() == 5u);
    CHECK(fws.contains(RenderPassSlot::Shadow));
}

TEST_CASE(b1_make_deferred_factory_path_is_deferred_passes_unchanged)
{
    // B1 stub frozen snapshot (2026-07-22 B1 commit `0292ea7`) —
    // makeDeferred() at B1 time tagged path=Deferred but returned
    // the SAME 5-slot Forward pipeline (intentional pre-wiring so
    // hosts / tests could pin the enum plumbing before B3).
    //
    // B3 SUPERSEDED this contract: makeDeferred() now returns a
    // 6-slot pipeline {Shadow, GBuffer, Lighting, Trans, PP, UI}
    // (no ForwardOpaque per cutsheet §4.1 red line #4). The B3
    // reality is pinned by Test_B3::b3_full_deferred_pipeline_noop_dispatch
    // (parts 1+2 — Forward 5-slot, Deferred 6-slot, FO omitted).
    //
    // This B1 case is RETAINED as a historical-record-of-B1-ship
    // contract — it pins only what B1 ship promised (the path tag).
    // Pass-list assertions are removed because B3 supersedes them.
    const RenderPipelineDesc def = RenderPipelineDesc::makeDefault();
    const RenderPipelineDesc deferred = RenderPipelineDesc::makeDeferred();

    // B1 contract still holds: path tag.
    CHECK(deferred.path == RenderPath::Deferred);
    CHECK(deferred.isDeferred() == true);
    // B3 SUPERSEDED: B1 ship size was 5u; B3 is 6u.
    // B3 SUPERSEDED: deferred.passes != def.passes (different lists).
    (void)def;  // silence unused warning if we remove all size/equal asserts
}

TEST_CASE(b1_configure_pipeline_with_deferred_desc_records_path)
{
    // B1.5 — configurePipeline(makeDeferred()) stores the path on
    // the Renderer; pipelineDesc().isDeferred() reads back true. The
    // pipeline dispatch is unchanged at B1 (same 5 passes execute
    // in the same order); only the recorded path differs.
    // B3 SUPERSEDED: makeDeferred() now returns 6 slots (not 5).
    // Pass-list size assertion is removed because B3 owns that fact
    // (Test_B3 case 7 parts 1+2). This case retains its historical
    // B1 contract: path tag is recorded + retrievable.
    Renderer renderer;
    CHECK(renderer.pipelineDesc().isDeferred() == false);  // default Forward

    renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
    CHECK(renderer.pipelineDesc().isDeferred() == true);
    CHECK(renderer.pipelineDesc().path == RenderPath::Deferred);
    // B3 SUPERSEDED: B1 ship size was 5u; B3 is 6u.

    // Re-configuring with Forward flips the tag back — hosts can
    // swap paths without rebuilding the Renderer.
    renderer.configurePipeline(RenderPipelineDesc::makeDefault());
    CHECK(renderer.pipelineDesc().isDeferred() == false);
}

TEST_CASE(b1_configure_pipeline_with_empty_desc_falls_back_to_forward)
{
    // B1.6 — Empty desc still means "use default" (per E5); it does
    // NOT mean "Deferred" (we'd need a sentinel and B3 to make
    // empty-desc=Deferred meaningful, which is a different design
    // — keep the contract simple here).
    Renderer renderer;
    renderer.configurePipeline(RenderPipelineDesc::makeDeferred());
    CHECK(renderer.pipelineDesc().isDeferred() == true);

    renderer.configurePipeline(RenderPipelineDesc{});
    CHECK(renderer.pipelineDesc().isDeferred() == false);
    CHECK(renderer.pipelineDesc().path == RenderPath::Forward);
}

TEST_CASE(b1_render_path_enum_is_pod_no_bgfx_leak)
{
    // B1.7 — ABI sanity: RenderPath is a uint8 enum (no bgfx types).
    // Trivially copyable so RenderPipelineDesc stays a POD-ish struct
    // (it has a std::vector already; this just guards against any
    // non-trivial member creeping in via RenderPath).
    static_assert(std::is_trivially_copyable<RenderPath>::value,
                  "RenderPath must be trivially copyable (plain enum).");
    static_assert(sizeof(RenderPath) == sizeof(uint8_t),
                  "RenderPath size must be 1 byte.");
    // Public header doesn't drag in bgfx (Test_PublicHeaderSurface
    // is the canonical gate; here we just confirm RenderPath itself
    // is a plain enum).
    static_assert(std::is_enum<RenderPath>::value,
                  "RenderPath must remain a scoped enum.");
}

TEST_SUITE_END