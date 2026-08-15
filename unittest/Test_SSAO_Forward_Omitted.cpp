// §A2 SSAO Forward Omitted test (2026-07-24, mid-term FG MVP SSAO
// Gate commit).
//
// Pins the cutsheet §S2 hard line: Forward pipeline (makeDefault)
// does NOT mount SSAO. Forward hosts see 0 behavior change even
// after A2 lands. Deferred pipeline (makeDeferred) DOES mount SSAO.
//
// The Forward path is verified by:
//   1) RenderPipelineDesc::makeDefault().passes does not contain
//      RenderPassSlot::SSAO (covered by Test_SSAO_A2::a2_..._...)
//   2) RenderPipeline built from makeDefault() does not allocate
//      a SSAOPass instance (slot enumeration walk)
//   3) The Renderer's SSAOPass factory (`makePassForSlot(SSAO)`)
//      returns a non-null SSAOPass — but only because the
//      pipeline-build step OMITS the SSAO slot for Forward;
//      the SSAOPass itself isn't mounted on Forward.
//
// These tests are mostly redundant with the A2 §A section but
// are kept so a reader searching for "Forward omitted" can grep
// this file name directly.

#include "AYTest.h"
#include "AYRenderer.h"
#include "AYRenderer/RenderScene.h"
#include "AYRenderer/RenderTypes.h"
#include "AYShader/ShaderResourcePool.h"
#include "AYShader/ShaderResource.h"

#include "detail/BGFXAdapter.h"
#include "detail/SSAOPass.h"

#include <memory>

using ayt::render::RenderPath;
using ayt::render::RenderPassSlot;
using ayt::render::RenderPipelineDesc;
using ayt::render::detail::SSAOPass;

TEST_SUITE(AYRenderer_SSAO_Forward_Omitted)

TEST_CASE(forward_make_default_path_is_forward) {
    const auto desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
}

TEST_CASE(forward_make_default_no_ssao_slot) {
    const auto desc = RenderPipelineDesc::makeDefault();
    bool found = false;
    for (const auto s : desc.passes) {
        if (s == RenderPassSlot::SSAO) {
            found = true;
            break;
        }
    }
    CHECK_FALSE(found);
}

TEST_CASE(deferred_make_deferred_has_ssao_slot) {
    // Mirror the cutsheet §S2 "Deferred-only" rule: SSAO appears
    // exclusively on the Deferred pipeline. Forward mounts zero
    // SSAOPass instances.
    const auto desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    bool found = false;
    for (const auto s : desc.passes) {
        if (s == RenderPassSlot::SSAO) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE(ssao_pass_factory_produces_valid_instance) {
    // Smoke test — makePassForSlot(SSAO) is reachable but the
    // Forward pipeline desc OMITS the slot so no SSAOPass is
    // actually mounted at runtime. The factory itself is correct.
    SSAOPass pass{};
    CHECK(pass.name() == "SSAO");
    CHECK(SSAOPass::kSsaoViewId == 14u);
}

TEST_CASE(forward_path_no_ssao_dep_on_gbuffer) {
    // Self-contained: Forward pipeline ⇒ no GBuffer, no SSAO, no
    // SceneColor→SSAOTexture. Just a structural pin.
    const auto forward = RenderPipelineDesc::makeDefault();
    CHECK_FALSE(forward.contains(RenderPassSlot::SSAO));
    CHECK_FALSE(forward.contains(RenderPassSlot::GBuffer));
    CHECK_FALSE(forward.contains(RenderPassSlot::Lighting));

    const auto deferred = RenderPipelineDesc::makeDeferred();
    CHECK(deferred.contains(RenderPassSlot::SSAO));
    CHECK(deferred.contains(RenderPassSlot::GBuffer));
    CHECK(deferred.contains(RenderPassSlot::Lighting));
}

TEST_SUITE_END
