#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ayt::render {
// Host-level deferred reflection controls; disabled means no target or draws.
struct SsrSettings {
    bool enabled = false;
    float strength = 1.0f;
    float maxDistance = 30.0f;
    float thickness = 0.2f;
    float maxRoughness = 0.6f;
    uint8_t steps = 48;
    uint8_t debugView = 0; // 0 composite, 1 hit confidence, 2 reflected radiance
};
inline SsrSettings sanitizeSsrSettings(SsrSettings s) noexcept {
    auto finite = [](float x, float fallback, float low, float high) {
        return std::isfinite(x) ? std::clamp(x, low, high) : fallback;
    };
    s.strength = finite(s.strength, 1, 0, 1);
    s.maxDistance = finite(s.maxDistance, 30, 0.1f, 200);
    s.thickness = finite(s.thickness, 0.2f, 0.001f, 2);
    s.maxRoughness = finite(s.maxRoughness, 0.6f, 0.05f, 0.95f);
    s.steps = std::clamp<uint8_t>(s.steps, 8, 64);
    s.debugView = std::min<uint8_t>(s.debugView, 2);
    return s;
}
}
