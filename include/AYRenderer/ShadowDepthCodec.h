#pragma once

#include "AYRenderer/ShadowSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ayt::render
{

// CPU mirror of bgfx example 16 shadow depth encoding / compare.
// GPU shaders in AYRenderer/ShadowShaderSources.h must stay in sync with this file.
struct ShadowDepthCodec {
    // bgfx fs_shadowmaps_packdepth: depth = v_position.z/v_position.w * 0.5 + 0.5
    // Requires clip z/w in NDC range (Phase 3 matrix builder).
    static constexpr float clipZOverWToNdc01(float zOverW) noexcept
    {
        return zOverW * 0.5f + 0.5f;
    }

    static constexpr float ndc01ToClipZOverW(float ndc01) noexcept
    {
        return (ndc01 - 0.5f) * 2.0f;
    }

    // bgfx shaderlib packFloatToRgba / unpackFloatFromRgba (RGBA8 shadow map).
    // Matches GLSL `res -= res.xxyz * bitMsk` (parallel swizzle, not sequential).
    static void packFloatToRgba(float value, float outRgba[4]) noexcept
    {
        const float bitSh[4] = {
            256.0f * 256.0f * 256.0f,
            256.0f * 256.0f,
            256.0f,
            1.0f,
        };
        const float inv256 = 1.0f / 256.0f;

        float res[4];
        for (int i = 0; i < 4; ++i) {
            res[i] = value * bitSh[i];
            res[i] = res[i] - std::floor(res[i]);
        }
        const float x = res[0];
        const float y = res[1];
        const float z = res[2];
        // xxyz → (x, x, y, z)
        outRgba[0] = x;
        outRgba[1] = y - x * inv256;
        outRgba[2] = z - y * inv256;
        outRgba[3] = res[3] - z * inv256;
    }

    static float unpackFloatFromRgba(const float rgba[4]) noexcept
    {
        const float bitSh[4] = {
            1.0f / (256.0f * 256.0f * 256.0f),
            1.0f / (256.0f * 256.0f),
            1.0f / 256.0f,
            1.0f,
        };
        return rgba[0] * bitSh[0] + rgba[1] * bitSh[1] + rgba[2] * bitSh[2]
             + rgba[3] * bitSh[3];
    }

    static float unpackFloatFromRgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept
    {
        const float rgba[4] = {
            static_cast<float>(r) / 255.0f,
            static_cast<float>(g) / 255.0f,
            static_cast<float>(b) / 255.0f,
            static_cast<float>(a) / 255.0f,
        };
        return unpackFloatFromRgba(rgba);
    }

    static float packUnpackRoundTrip(float ndc01) noexcept
    {
        float rgba[4] = {};
        packFloatToRgba(ndc01, rgba);
        return unpackFloatFromRgba(rgba);
    }

    static float predictLitFactor(float refNdc01,
                                  float occluderNdc01,
                                  float shadowBias = ShadowSettings::kBiasDefault) noexcept
    {
        const auto step = [](float edge, float x) -> float {
            return x >= edge ? 1.0f : 0.0f;
        };
        const float cleared = step(0.999f, occluderNdc01) * step(occluderNdc01, 1.001f);
        const float inShadow = step(occluderNdc01 + shadowBias, refNdc01);
        const float compared = 1.0f - inShadow;
        return std::fmax(compared, cleared);
    }
};

// Legacy entry points (Phase 1 — semantics changed from (z-near)/range to ndc01).
inline float encodeShadowDepth(float rawClipZOverW) noexcept
{
    return ShadowDepthCodec::clipZOverWToNdc01(rawClipZOverW);
}

inline float predictShadowLitFactor(float refDepth,
                                    float occluder,
                                    float shadowBias = ShadowSettings::kBiasDefault) noexcept
{
    return ShadowDepthCodec::predictLitFactor(refDepth, occluder, shadowBias);
}

} // namespace ayt::render
