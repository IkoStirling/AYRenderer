#include "AYRenderer/ShadowDiagnostics.h"
#include "AYRenderer/ShadowSettings.h"

#include "AYTest.h"

#include <string_view>

using ayt::render::ShadowDiagnostics;
using ayt::render::ShadowFrameStats;
using ayt::render::ShadowLogLevel;
using ayt::render::ShadowSettings;

TEST_SUITE(ShadowDiagnostics)

TEST_CASE(log_level_enum_ordered)
{
    CHECK(static_cast<uint8_t>(ShadowLogLevel::L0_Silent) <
          static_cast<uint8_t>(ShadowLogLevel::L1_Caps));
    CHECK(static_cast<uint8_t>(ShadowLogLevel::L1_Caps) <
          static_cast<uint8_t>(ShadowLogLevel::L2_Frame));
    CHECK(static_cast<uint8_t>(ShadowLogLevel::L2_Frame) <
          static_cast<uint8_t>(ShadowLogLevel::L3_Probe));
    CHECK(static_cast<uint8_t>(ShadowLogLevel::L3_Probe) <
          static_cast<uint8_t>(ShadowLogLevel::L4_Verbose));
}

TEST_CASE(enabled_respects_need_vs_current)
{
    // Without env manipulation we can still check the comparison helper
    // against an explicit need level relative to whatever env is set.
    const ShadowLogLevel current = ShadowDiagnostics::levelFromEnv();
    CHECK(ShadowDiagnostics::enabled(ShadowLogLevel::L0_Silent));
    if (current == ShadowLogLevel::L0_Silent) {
        CHECK(ShadowDiagnostics::enabled(ShadowLogLevel::L1_Caps) == false);
    } else {
        CHECK(ShadowDiagnostics::enabled(ShadowLogLevel::L1_Caps)
              == (static_cast<uint8_t>(current) >= 1u));
    }
}

TEST_CASE(frame_stats_pod_defaults)
{
    ShadowFrameStats s{};
    CHECK(s.frameIndex == 0u);
    CHECK(s.atlasSlots == 0u);  // §S2-3 — renamed from casterDraws
    CHECK(s.casterReady == false);
    CHECK(s.sampleReady == false);
}

TEST_CASE(build_stamp_is_phase6)
{
    CHECK(std::string_view(ShadowSettings::kPipelineBuildStamp)
          == std::string_view("v13-phase7-vec4-abi"));
}

TEST_SUITE_END
