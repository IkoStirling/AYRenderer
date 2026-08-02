#pragma once

#include "AYShadowSettings.h"

#include <ayio/Env.h>
#include <cstdint>
#include <string>

namespace ayt::render
{

// Phase 6 — shadow diagnostic verbosity.
// Controlled by AY_SHADOW_LOG (0–4). Feature toggles stay separate:
//   AY_SHADOW_DEBUG=1         → receiver debugVis (shadow map grayscale)
//   AY_SHADOW_CASTER_SOLID=1  → caster writes constant 0.5
enum class ShadowLogLevel : uint8_t {
    L0_Silent   = 0, // production default
    L1_Caps     = 1, // once: build stamp + bgfx caps
    L2_Frame    = 2, // first N frames: ShadowPass summary line
    L3_Probe    = 3, // CPU LVP probes (cube/ground UV + refN)
    L4_Verbose  = 4, // cast draws + ShadowBind per-draw spam
};

struct ShadowFrameStats {
    uint32_t frameIndex   = 0;
    uint32_t atlasSlots   = 0;  // §S2-3 — renamed from casterDraws:
                                // the count is "atlas sub-rect slots
                                // used", NOT mesh draw-call count.
                                // (pre-C was always 1, post-§P5.5 C
                                // can be N up to kMaxShadowCasters.)
    bool     casterReady  = false;
    bool     fboValid     = false;
    bool     blitOk       = false;
    bool     sampleReady  = false;
    float    probeU       = 0.0f;
    float    probeV       = 0.0f;
    float    expectEnc    = 0.0f;
    uint16_t probePxX     = 0;
    uint16_t probePxY     = 0;
    uint16_t resolveIdx   = 0;
    uint16_t rtIdx        = 0;
};

struct ShadowDiagnostics {
    static constexpr uint32_t kFrameSummaryLimit = 8;
    static constexpr uint32_t kProbeLogLimit     = 5;
    static constexpr uint32_t kVerboseLogLimit   = 8;

    // Default L2 so Editor always prints the first-N-frame ShadowPass summary
    // (atlasSlots / sampleReady / probe) without requiring AY_SHADOW_LOG.
    // §S2-3 (2026-07-23) — "atlasSlots" replaces "draws" in the summary
    // log key to avoid the long-running misconception that the number
    // is mesh-draw-call count.
    static ShadowLogLevel levelFromEnv() noexcept
    {
        // Wrap env::get in a try/catch to preserve the previous
        // noexcept contract — the underlying call allocates a
        // std::string on Windows and could throw std::bad_alloc on
        // the rare alloca-fail path. Drop back to the production
        // default L2_Frame if anything goes wrong.
        std::string env;
        try {
            env = ayt::io::env::get("AY_SHADOW_LOG").value_or("");
        } catch (...) {
            return ShadowLogLevel::L2_Frame;
        }
        if (env.empty()) {
            if (ayt::io::env::get("AY_SHADOW_DEBUG").has_value()) {
                return ShadowLogLevel::L3_Probe;
            }
            return ShadowLogLevel::L2_Frame;
        }
        if (env[0] >= '0' && env[0] <= '4' && env[1] == '\0') {
            return static_cast<ShadowLogLevel>(env[0] - '0');
        }
        // Accept L0..L4 / l0..l4
        if ((env[0] == 'L' || env[0] == 'l')
            && env[1] >= '0' && env[1] <= '4'
            && env[2] == '\0') {
            return static_cast<ShadowLogLevel>(env[1] - '0');
        }
        return ShadowLogLevel::L1_Caps;
    }

    static bool enabled(ShadowLogLevel need) noexcept
    {
        return static_cast<uint8_t>(levelFromEnv()) >= static_cast<uint8_t>(need);
    }

    static bool debugVisEnabled() noexcept
    {
        // Preserve the v0 contract: enabled iff set AND not empty
        // AND first char != '0'. env::get() allocates on Windows —
        // catch the bad_alloc path so noexcept stays intact.
        std::string env;
        try {
            env = ayt::io::env::get("AY_SHADOW_DEBUG").value_or("");
        } catch (...) {
            return false;
        }
        return !env.empty() && env[0] != '0';
    }

    static bool casterSolidEnabled() noexcept
    {
        std::string env;
        try {
            env = ayt::io::env::get("AY_SHADOW_CASTER_SOLID").value_or("");
        } catch (...) {
            return false;
        }
        return !env.empty() && env[0] != '0';
    }
};

} // namespace ayt::render
