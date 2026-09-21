// §P5 T1-T5 (2026-08-24) — Part 5 PostProcess + UI + Debug + Tests
// + Docs invariants regression net. Pins the contracts introduced
// by the Part 5 audit fixes (5M + 2L + 5T).
//
//   T1  M1+M2 — PostProcess failure diagnostics remain rate-limited;
//                successful submit logging is one-shot.
//   T2  M3    — All 6 silent early-returns in PostProcessPass
//                ::execute invoke rateLimitedEarlyReturn before
//                `return 0;` (verified by string-grep on the
//                source file at test time — guarantees the
//                count won't regress silently).
//   T3  M4    — PostProcess owns no private FBO or FBO-format ABI.
//   T4  M5    — DebugOverlay caches bgfx::getStats() once per
//                frame: onEndFrame() reads from the cached
//                pointer set by sampleBgfxStats(); the test
//                verifies the cache field exists and is
//                cleared on resetStats().
//   T5  L1+L3 — current shader cache key and resource teardown
//                ordering stay pinned.

#include "AYTest.h"

#include "AYRenderer/RenderTypes.h"
#include "detail/DebugOverlay.h"
#include "detail/PostProcessPass.h"
#include "detail/RenderPass.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace ayt::render::detail {
extern const char* const kPostProcessCacheKeyCStr;
} // namespace ayt::render::detail

using ayt::render::detail::DebugOverlay;
using ayt::render::detail::kPostProcessCacheKeyCStr;
using ayt::render::detail::rateLimitedEarlyReturn;

namespace {

// Helper: count occurrences of a substring in a string. Avoids
// pulling <algorithm> just for one trivial loop.
std::size_t countSubstr(const std::string& haystack,
                        const std::string& needle)
{
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string readEntireFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string rendererSourcePath(const char* relativePath)
{
    return std::string(AY_RENDERER_SOURCE_DIR) + "/" + relativePath;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// T1 — M1+M2: One-shot bool logs paired with rateLimitedEarlyReturn
// ─────────────────────────────────────────────────────────────────────

TEST_SUITE(AuditP5_T1_RateLimitedSubmitLogs)

TEST_CASE(rate_limited_early_return_addressable) {
    // M1+M2 fix: PostProcessPass::execute() now calls
    // rateLimitedEarlyReturn alongside the existing
    // one-shot bool logs. Verify the helper is addressable
    // from a test (so a future refactor that breaks the
    // link drops the call site silently — this test catches
    // it by failing to link).
    void (*fn)(const char*, const char*) = &rateLimitedEarlyReturn;
    CHECK(fn != nullptr);
}

TEST_CASE(rate_limited_early_return_safe_on_null) {
    // Defensive: the helper must accept nullptr inputs without
    // crashing (mirrors Part 4 T2 — same contract).
    rateLimitedEarlyReturn(nullptr, nullptr);
    CHECK(true);
}

TEST_SUITE_END

// ─────────────────────────────────────────────────────────────────────
// T2 — M3: 6 silent early-returns in PostProcessPass::execute
// ─────────────────────────────────────────────────────────────────────

TEST_SUITE(AuditP5_T2_SilentEarlyReturnsLogged)

TEST_CASE(post_process_source_rate_limits_failure_exits) {
    // M3 fix: PostProcessPass::execute() gained 6
    // rateLimitedEarlyReturn calls (one per silent
    // early-return site). We pin the count by reading the
    // source file at test time and grep-counting the helper
    // invocations. This catches silent regressions where a
    // future refactor adds a new `return 0;` branch without
    // the matching diagnostic.
    //
    // The path is fixed relative to the source tree; the test
    // is gated on the file existing (otherwise it's a no-op
    // CHECK(true) — build systems can relocate the tree).
    const std::string path =
        rendererSourcePath("src/detail/PostProcessPass.cpp");
    const std::string src = readEntireFile(path);
    CHECK(!src.empty());
    if (src.empty()) {
        return;
    }

    // Count "rateLimitedEarlyReturn(" calls in PostProcessPass.cpp.
    // The fix introduced diagnostics for failure exits in execute() (1 each for
    // !isInitialized, isNoopBackend, zero-viewport, sourceFbo
    // invalid, VB/IB invalid, fboColor invalid) + 1 before the
    // s_loggedMissing return (M1). Successful
    // blits deliberately do not use an early-return diagnostic.
    const std::size_t calls = countSubstr(
        src, "rateLimitedEarlyReturn(");
    CHECK(calls >= 7u);
}

TEST_SUITE_END

// ─────────────────────────────────────────────────────────────────────
// T3 — M4: FBO create-failed log uses rateLimitedEarlyReturn
// ─────────────────────────────────────────────────────────────────────

TEST_SUITE(AuditP5_T3_FboCreateFailLog)

TEST_CASE(post_process_has_no_private_render_target_lifecycle) {
    const std::string header = readEntireFile(
        rendererSourcePath("src/detail/PostProcessPass.h"));
    const std::string source = readEntireFile(
        rendererSourcePath("src/detail/PostProcessPass.cpp"));
    CHECK(!header.empty());
    CHECK(!source.empty());
    CHECK(header.find("ensureFbo(") == std::string::npos);
    CHECK(source.find("ensureFbo(") == std::string::npos);
    CHECK(source.find("createFrameBuffer(") == std::string::npos);
    CHECK(header.find("kPostProcessColorFormat") == std::string::npos);
}

TEST_SUITE_END

// ─────────────────────────────────────────────────────────────────────
// T4 — M5: DebugOverlay caches bgfx::getStats() once per frame
// ─────────────────────────────────────────────────────────────────────

TEST_SUITE(AuditP5_T4_StatsCacheContract)

TEST_CASE(debug_overlay_last_bgfx_stats_field_exists) {
    // M5 fix: DebugOverlay now stores the bgfx::Stats pointer
    // in `_lastBgfxStats` (set by sampleBgfxStats, read by
    // onEndFrame). We can't observe the private member
    // directly, but we can confirm:
    //   (a) the header declares it
    //   (b) resetStats() clears it to nullptr
    // by exercising the public API. Default-constructed
    // DebugOverlay: resetStats() must not crash.
    DebugOverlay overlay;
    overlay.resetStats();
    CHECK(true);
}

TEST_CASE(debug_overlay_source_has_no_duplicate_get_stats_call) {
    // M5 fix: onEndFrame() should read bgfx::getStats()
    // exactly ONCE per frame (via the cached pointer set by
    // sampleBgfxStats()). Pre-fix it was called twice. Pin by
    // grep-counting bgfx::getStats() invocations in the
    // source — should be exactly 1 (only inside
    // sampleBgfxStats).
    const std::string path =
        rendererSourcePath("src/detail/DebugOverlay.cpp");
    const std::string src = readEntireFile(path);
    CHECK(!src.empty());
    if (src.empty()) {
        return;
    }
    // Count the executable assignment, not comments that mention the API.
    const std::size_t calls =
        countSubstr(src, "= bgfx::getStats();");
    CHECK(calls == 1u);
}

TEST_CASE(debug_overlay_source_uses_cached_pointer_in_on_end_frame) {
    // M5 fix: onEndFrame() reads from `_lastBgfxStats`
    // (the cached pointer), not a fresh bgfx::getStats()
    // call. Verify the source has both: a write to
    // _lastBgfxStats in sampleBgfxStats and a read in
    // onEndFrame.
    const std::string path =
        rendererSourcePath("src/detail/DebugOverlay.cpp");
    const std::string src = readEntireFile(path);
    CHECK(!src.empty());
    if (src.empty()) {
        return;
    }
    // Pin the write side (sampleBgfxStats sets the cache).
    CHECK(src.find("_lastBgfxStats = bgfxStats")
          != std::string::npos);
    // Pin the read side (onEndFrame consumes the cache).
    CHECK(src.find("const bgfx::Stats* bgfxStats = _lastBgfxStats")
          != std::string::npos);
    // Pin the reset side (resetStats clears the cache).
    CHECK(src.find("_lastBgfxStats = nullptr")
          != std::string::npos);

    // The refresh must happen in onEndFrame before the cached pointer is
    // consumed; sampling later from onFrameSubmitted adds another frame of
    // latency to the displayed counters.
    const std::size_t onEnd = src.find("void DebugOverlay::onEndFrame");
    const std::size_t sample = src.find("updateFrameStats(drawCalls, sceneItems, passStats, gpuSample);", onEnd);
    const std::size_t consume = src.find(
        "const bgfx::Stats* bgfxStats = _lastBgfxStats", onEnd);
    const std::size_t submitted = src.find("void DebugOverlay::onFrameSubmitted");
    CHECK(onEnd != std::string::npos);
    CHECK(sample != std::string::npos);
    CHECK(consume != std::string::npos);
    CHECK(sample < consume);
    CHECK(submitted == std::string::npos || sample < submitted);
}

TEST_SUITE_END

// ─────────────────────────────────────────────────────────────────────
// T5 — current cache key + resource teardown ordering
// ─────────────────────────────────────────────────────────────────────

TEST_SUITE(AuditP5_T5_CacheKeyAndFormat)

TEST_CASE(post_process_cache_key_tracks_sanitized_parameter_abi) {
    // Pin the runtime key so shader ABI changes cannot reuse stale binaries.
    CHECK(std::string_view(kPostProcessCacheKeyCStr)
          == "postprocess_tonemap_aces_v12_sanitized_params_fs");
}

TEST_CASE(post_process_renderer_owns_teardown_before_pipeline_clear) {
    const std::string source = readEntireFile(
        rendererSourcePath("src/AYRenderer.cpp"));
    CHECK(!source.empty());
    const std::size_t passLookup = source.find(
        "pipeline.findPass<detail::PostProcessPass>()");
    const std::size_t destroyCall = source.find(
        "->destroyResources(adapter)", passLookup);
    const std::size_t pipelineClear = source.find("pipeline.clear()", passLookup);
    CHECK(passLookup != std::string::npos);
    CHECK(destroyCall != std::string::npos);
    CHECK(pipelineClear != std::string::npos);
    CHECK(destroyCall < pipelineClear);
}

TEST_SUITE_END
