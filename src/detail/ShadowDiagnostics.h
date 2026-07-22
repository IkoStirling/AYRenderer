#pragma once

#include "AYShadowDiagnostics.h"

#include <cstdio>

namespace ayt::render::detail
{

inline void logShadowCapsIfNeeded(uint32_t rendererType,
                                  bool capsBlit,
                                  bool capsReadBack)
{
    if (!ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L1_Caps)) {
        return;
    }
    static bool logged = false;
    if (logged) {
        return;
    }
    logged = true;
    std::fprintf(stderr,
                 "[ShadowPass] build=%s capsBlit=%d capsReadBack=%d renderer=%u logLevel=%u\n",
                 ayt::render::kShadowPipelineBuildStamp,
                 capsBlit ? 1 : 0,
                 capsReadBack ? 1 : 0,
                 static_cast<unsigned>(rendererType),
                 static_cast<unsigned>(ayt::render::ShadowDiagnostics::levelFromEnv()));
}

inline void logShadowFrameStatsIfNeeded(const ayt::render::ShadowFrameStats& s)
{
    if (!ayt::render::ShadowDiagnostics::enabled(ayt::render::ShadowLogLevel::L2_Frame)) {
        return;
    }
    if (s.frameIndex > ayt::render::ShadowDiagnostics::kFrameSummaryLimit) {
        return;
    }
    std::fprintf(stderr,
                 "[ShadowPass] frame=%u build=%s casterReady=%d draws=%u "
                 "fboValid=%d rt.idx=%u resolve.idx=%u blitOk=%d "
                 "sampleReady=%d probeUv=(%.4f,%.4f) expectEnc=%.4f probePx=(%u,%u)\n",
                 s.frameIndex,
                 ayt::render::kShadowPipelineBuildStamp,
                 s.casterReady ? 1 : 0,
                 s.casterDraws,
                 s.fboValid ? 1 : 0,
                 static_cast<unsigned>(s.rtIdx),
                 static_cast<unsigned>(s.resolveIdx),
                 s.blitOk ? 1 : 0,
                 s.sampleReady ? 1 : 0,
                 s.probeU,
                 s.probeV,
                 s.expectEnc,
                 static_cast<unsigned>(s.probePxX),
                 static_cast<unsigned>(s.probePxY));
}

} // namespace ayt::render::detail
