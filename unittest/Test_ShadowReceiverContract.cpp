#include "AYRenderer/ShadowConfig.h"
#include "AYTest.h"

#include <string_view>

using ayt::render::ShadowFlags;
using ayt::render::ShadowReceiverContract;
using ayt::render::ShadowSettings;
using ayt::render::kShadowCastAndReceive;
using ayt::render::makeShadowFlags;
using ayt::render::receivesShadow;

TEST_SUITE(ShadowReceiverContract)

TEST_CASE(contract_names_match_phoskia_receiver)
{
    CHECK(ShadowReceiverContract::kShadowMapName == "shadowMap");
    CHECK(ShadowReceiverContract::kLightViewProjName == "u_lightViewProj");
    CHECK(ShadowReceiverContract::kLightViewProjAlt == "lightViewProj");
    CHECK(ShadowReceiverContract::kShadowBiasName == "shadowBias");
    CHECK(ShadowReceiverContract::kShadowDebugName == "shadowDebugVis");
    CHECK(ShadowReceiverContract::kShadowMapTexelName == "shadowMapTexel");
    CHECK(ShadowReceiverContract::kShadowPcfName == "shadowPcf");
    CHECK(ShadowReceiverContract::kShadowSamplerStage == 1u);
}

TEST_CASE(should_sample_honors_receive_flag)
{
    CHECK(ShadowReceiverContract::shouldSampleShadowMap(kShadowCastAndReceive));
    CHECK(ShadowReceiverContract::shouldSampleShadowMap(ShadowFlags::Receive));
    CHECK(ShadowReceiverContract::shouldSampleShadowMap(ShadowFlags::Cast) == false);
    CHECK(ShadowReceiverContract::shouldSampleShadowMap(ShadowFlags::None) == false);
    CHECK(ShadowReceiverContract::shouldSampleShadowMap(
              makeShadowFlags(/*cast=*/true, /*receive=*/false))
          == false);
    CHECK(ShadowReceiverContract::shouldSampleShadowMap(
              makeShadowFlags(/*cast=*/false, /*receive=*/true)));
}

TEST_CASE(receive_helpers_align_with_mesh_component)
{
    // MeshComponent ground: cast=false, receive=true
    const ShadowFlags ground = makeShadowFlags(false, true);
    CHECK(receivesShadow(ground));
    CHECK(ShadowReceiverContract::shouldSampleShadowMap(ground));

    // MeshComponent cube: cast=true, receive=true
    const ShadowFlags cube = makeShadowFlags(true, true);
    CHECK(receivesShadow(cube));
    CHECK(ShadowReceiverContract::shouldSampleShadowMap(cube));
}

TEST_CASE(default_bias_matches_settings)
{
    CHECK(ShadowReceiverContract::kDefaultBias == ShadowSettings::kBiasDefault);
}

TEST_SUITE_END
