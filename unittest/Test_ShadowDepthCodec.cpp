#include "AYShadowDepthCodec.h"

#include "AYTest.h"

#include <cmath>

using ayt::render::ShadowDepthCodec;
using ayt::render::encodeShadowDepth;

TEST_SUITE(AYShadowDepthCodec)

TEST_CASE(ndc01_clip_z_over_w_matches_bgfx_packdepth)
{
    CHECK(std::fabs(ShadowDepthCodec::clipZOverWToNdc01(0.0294f) - 0.5147f) < 0.001f);
    CHECK(std::fabs(ShadowDepthCodec::clipZOverWToNdc01(0.0314f) - 0.5157f) < 0.001f);
    CHECK(std::fabs(ShadowDepthCodec::ndc01ToClipZOverW(0.5147f) - 0.0294f) < 0.001f);
}

TEST_CASE(pack_unpack_rgba_round_trip)
{
    const float ndc01 = 0.5157f;
    const float roundTrip = ShadowDepthCodec::packUnpackRoundTrip(ndc01);
    CHECK(std::fabs(roundTrip - ndc01) < 0.002f);
}

TEST_CASE(pack_unpack_rgba8_contact_depths)
{
    float cubeRgba[4] = {};
    float groundRgba[4] = {};
    ShadowDepthCodec::packFloatToRgba(0.5147f, cubeRgba);
    ShadowDepthCodec::packFloatToRgba(0.5157f, groundRgba);

    const float cubeEnc = ShadowDepthCodec::unpackFloatFromRgba(cubeRgba);
    const float groundEnc = ShadowDepthCodec::unpackFloatFromRgba(groundRgba);

    CHECK(std::fabs(cubeEnc - 0.5147f) < 0.002f);
    CHECK(std::fabs(groundEnc - 0.5157f) < 0.002f);

    const float litAtGround =
        ShadowDepthCodec::predictLitFactor(groundEnc, cubeEnc, 0.001f);
    CHECK(litAtGround < 0.05f);
}

TEST_CASE(cleared_map_stays_lit)
{
    const float groundEnc = 0.5157f;
    const float lit = ShadowDepthCodec::predictLitFactor(groundEnc, 1.0f);
    CHECK(lit > 0.99f);
}

TEST_CASE(legacy_encodeShadowDepth_alias_uses_ndc01)
{
    CHECK(std::fabs(encodeShadowDepth(0.0314f) - 0.5157f) < 0.001f);
}

TEST_SUITE_END
