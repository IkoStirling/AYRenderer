// PR-P4.2 (2026-07-22) — §P4 bias 精修 plumbing smoke tests.
//
// Background (§P4.2 in docs/execution-plan.md + docs/shadow-pass.md):
//
//   The Phoskia receiver shader already had a `shadowBias` property
//   (default 0.003) since PR-F1'. What P4.2 ships is the CPU mirror:
//   FrameContext::shadowBias + Renderer::setShadowBias/getter +
//   tryBindShadowSampler signature accepts the bias value so the
//   host can tune it in one place for every receiver material.
//
// What this suite pins:
//   1. Renderer::shadowBias() default = 0.003f (matches Phoskia
//      property default + ShadowSettings::kBiasDefault).
//   2. setShadowBias stores the value; getter returns it.
//   3. FrameContext::shadowBias is initialized to 0.003f by default.
//   4. tryBindShadowSampler accepts the new `bias` parameter
//      without ABI break on existing call sites (default arg
//      keeps old signature compatible).
//
// Out-of-scope (covered by E5 / R5+):
//   - Visual bias tuning on a live GPU backend (Noop short-circuits
//     before bias matters).
//   - Caster-side bias (currently no caster bias; receiver bias
//     alone is the suppression mechanism per lessons §3.6).

#include "AYRenderer.h"
#include "AYRenderTypes.h"
#include "AYTest.h"

#include "detail/FrameContext.h"
#include "detail/RenderPass.h"
#include "detail/ShadowPass.h"

#include "AYShaderResourcePool.h"
#include "AYShaderResource.h"

#include "AYMath/MathTypes.h"

#include <cstdint>
#include <memory>

using ayt::render::Renderer;
using ayt::render::detail::FrameContext;
using ayt::render::detail::ShadowPass;
using ayt::render::detail::tryBindShadowSampler;

TEST_SUITE(AYRenderer_P4_ShadowBias)

TEST_CASE(p4_shadow_bias_default_matches_phoskia_property)
{
    // P4.2.1 — FrameContext default 0.003f matches the Phoskia
    // receiver property default (AYShadowShaderSources.h:81) and
    // ShadowSettings::kBiasDefault. A drift here would mean existing
    // receivers render with a different bias than the host expects
    // (subtle acne / peter-panning regression).
    FrameContext frame;
    CHECK(frame.shadowBias > 0.0029f);
    CHECK(frame.shadowBias < 0.0031f);
}

TEST_CASE(p4_renderer_shadow_bias_getter_default)
{
    // P4.2.2 — Renderer::shadowBias() returns 0.003f by default
    // even before initialize(); the Impl field has a default value.
    Renderer renderer;
    CHECK(renderer.shadowBias() > 0.0029f);
    CHECK(renderer.shadowBias() < 0.0031f);
}

TEST_CASE(p4_renderer_set_shadow_bias_round_trip)
{
    // P4.2.3 — setShadowBias stores the value and the getter
    // returns it. Noop backend stores the same as a real backend
    // (the bias value lives on Impl, not in any GPU resource).
    Renderer renderer;
    renderer.setShadowBias(0.005f);
    CHECK(renderer.shadowBias() > 0.0049f);
    CHECK(renderer.shadowBias() < 0.0051f);

    renderer.setShadowBias(0.0f);
    CHECK(renderer.shadowBias() == 0.0f);

    // Host responsibility: negative bias accepted but discouraged;
    // no clamping per setMaterialFloat / setMaterialVec3 leniency.
    renderer.setShadowBias(-0.001f);
    CHECK(renderer.shadowBias() < 0.0f);
}

TEST_CASE(p4_try_bind_shadow_sampler_accepts_bias_param)
{
    // P4.2.4 — tryBindShadowSampler now takes a 5th `bias` param
    // with default 0.003f. Calling with explicit value compiles and
    // does not crash on Noop (shadowPass=nullptr ⇒ early-out at
    // the `shadowBinding == InvalidBinding` check since the helper
    // is called on a fresh ShaderResource with no bindings).
    ayt::shader::ShaderResourcePool pool;
    ayt::shader::ShaderResource shader;
    ayt::render::detail::BGFXAdapter adapter;

    // Noop backend, nullptr shadowPass: helper should early-return
    // without crashing (bias value irrelevant on the early-out path).
    tryBindShadowSampler(shader, adapter, /*shadowPass=*/nullptr,
                         ayt::render::kShadowCastAndReceive, /*bias=*/0.004f);

    // Also exercise the default-arg call site (older code paths).
    tryBindShadowSampler(shader, adapter, /*shadowPass=*/nullptr,
                         ayt::render::kShadowCastAndReceive);
    CHECK(true);
}

TEST_SUITE_END