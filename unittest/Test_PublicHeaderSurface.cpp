// Test_PublicHeaderSurface.cpp — public headers must not pull in bgfx

#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"

#include "AYTest.h"

TEST_SUITE(RendererPublicHeaderTests)

TEST_CASE(public_headers_compile_without_bgfx)
{
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    ayt::render::Renderer renderer;
    CHECK(!renderer.isInitialized());
    (void)desc;
}

TEST_CASE(public_renderer_symbols_resolve_at_runtime)
{
    // E5 (§5.4, 2026-07-22) — pin that the new
    // Renderer::shadowsEnabled() const noexcept getter is reachable
    // from the public ABI (not stripped by an inline-only path or
    // hidden behind a non-public include). Taking the address of
    // a member function forces a real out-of-line reference; if
    // the symbol ever disappears from the lib, this fails to link.
    using ShadowsEnabledFn = bool (ayt::render::Renderer::*)() const noexcept;
    ShadowsEnabledFn fn = &ayt::render::Renderer::shadowsEnabled;
    CHECK(fn != nullptr);
    ayt::render::Renderer renderer;
    CHECK((renderer.*fn)() == true);  // E5 default: enabled
}

TEST_CASE(public_renderer_lighting_enabled_getter_resolves)
{
    // §P5 B3 (2026-07-22) — pin that the new
    // Renderer::lightingEnabled() const noexcept getter is reachable
    // from the public ABI. Mirrors the shadowsEnabled() gate (E5).
    // Const noexcept getter; reads pipeline.findPass("Lighting") +
    // isEnabled() (no mirror field). Forward default ctor ⇒ no
    // Lighting slot mounted ⇒ returns false. After
    // configurePipeline(makeDeferred()), returns true.
    using LightingEnabledFn = bool (ayt::render::Renderer::*)() const noexcept;
    LightingEnabledFn fn = &ayt::render::Renderer::lightingEnabled;
    CHECK(fn != nullptr);

    ayt::render::Renderer renderer;
    CHECK((renderer.*fn)() == false);  // B3 default Forward: no Lighting slot
    renderer.configurePipeline(ayt::render::RenderPipelineDesc::makeDeferred());
    CHECK((renderer.*fn)() == true);   // after Deferred config: Lighting slot mounted
}

// §P5.5 C (2026-07-23) — Light POD size budget guard. Widened from
// ≤96 to ≤128 with the addition of castShadow + shadowBias fields
// (~80B actual). The static_assert in AYRenderScene.h:256 is the
// compile-time guard; this test is the runtime pin so any future
// cuts that try to widen past 128 trip both gates.
TEST_CASE(public_light_pod_size_post_c_widen_under_128) {
    using ayt::render::Light;
    const size_t sz = sizeof(Light);
    CHECK(sz > 0u);
    CHECK(sz <= 128u);   // §P5.5 C cap (was ≤96 pre-C)
    CHECK(sz >= 64u);    // sanity — pre-B was 40B, C widens to ~80B
}

TEST_SUITE_END
