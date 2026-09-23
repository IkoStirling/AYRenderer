#pragma once

#include "detail/PassExecContext.h"
#include "detail/RenderPass.h"
#include "detail/FullscreenPassGeometry.h"

#include "AYShader/ShaderResource.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string_view>

namespace ayt::render::detail
{

// Final fullscreen composite. It consumes the shared current-frame scene-color
// route (Forward sceneFbo, Deferred LightingOutput, or produced HazeColor),
// combines optional produced BloomSource, applies exposure + tone mapping +
// display gamma, and writes FrameGraph FinalLdrColor. PresentPass owns the
// later default-backbuffer boundary.
//
// PostProcess owns no render target. FrameGraph owns FinalLdrColor; this pass
// owns only reusable fullscreen geometry and one ShaderResource. An
// independent one-texture blit is retained as a compile fallback, while the
// primary program is retried at a bounded cadence.
class PostProcessPass final : public RenderPass {
public:
    // Stable final-composite view. Deferred ordering is SSAO(14) →
    // Lighting(8) → DepthHaze(13) → transparent/bloom → PostProcess(15),
    // with UI chrome fixed at view 255.
    static constexpr uint8_t kBlitViewId = 15;

    // Keep lifecycle symbols out-of-line. PostProcessPass is private engine
    // detail, but it is instantiated by several test translation units; an
    // inline default constructor can be COMDAT-folded from a stale object
    // after this class layout changes and then write past the caller's stack
    // allocation. A single implementation also gives layout changes one ABI
    // boundary during incremental Visual Studio builds.
    PostProcessPass();
    ~PostProcessPass() override;

    std::string_view name() const override { return "PostProcess"; }

    uint32_t execute(PassExecContext& ctx) override;

    // Compatibility wrapper. New consumers call
    // selectSceneColorSourceFbo() from SceneColorPipeline directly.
    static bgfx::FrameBufferHandle selectSourceFbo(const PassExecContext& ctx) noexcept;

    bool isReady() const noexcept {
        return _program.isValid()
            && _fullscreen.isReady();
    }

    // Must run while the adapter and shader pool are alive. Renderer invokes
    // it before pipeline replacement and before adapter shutdown.
    void destroyResources(BGFXAdapter& adapter);

private:
    FullscreenPassGeometry _fullscreen;

    // Lazily acquired primary or minimal fallback program.
    ayt::shader::ShaderResource _program;

    // R5.1 — cached uniform / texture binding IDs. Resolved from
    // _program on first acquire (cheaper than getUniformBinding every
    // frame); InvalidBinding (0) means "not yet resolved". Tests use
    // these to pin that the wire path actually found the names.
    ayt::shader::BindingId      _uBloomStrength = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uExposure      = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uAutoExposureEnabled = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uTonemapMode   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _uGammaParams   = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tSceneColor    = ayt::shader::InvalidBinding;
    // §S1c (2026-07-23, short-term-plan §S1 sub-cut 3) — second
    // sampler on the fullscreen-triangle composite draw, bound to
    // `ctx.bloomBlurPass->pongFbo()` RT0 when present. Replaces
    // the pre-S1 fake `raw + raw*bloomStrength` shader hack with
    // `raw + sample(bloomTexture, uv) * bloomStrength`. Invalid
    // when the program hasn't been acquired yet (mirror _tSceneColor).
    ayt::shader::BindingId      _tBloomTexture  = ayt::shader::InvalidBinding;
    ayt::shader::BindingId      _tAutoExposureTexture = ayt::shader::InvalidBinding;
    enum class ProgramVariant : uint8_t {
        None,
        Primary,
        Fallback,
    };
    ProgramVariant              _programVariant = ProgramVariant::None;
    // Failed compilation retries periodically instead of poisoning this pass
    // forever. The delay prevents shaderc from stuttering every frame.
    uint16_t                    _programRetryFrames = 0;

    // Helpers are no-ops on the Noop backend (BGFXAdapter
    // gates on isInitialized()), so the headless test path runs clean.
    void ensureProgram(shader::ShaderResourcePool& pool);
};

// §S4c (2026-07-23) — Bug fix #3 mirror (see DepthHazePass.h:154-167
// for the most-recent previous application, mirrored by
// BloomExtractPass.h, BloomBlurPass.h:185-199, LightingPass.h).
// Externalize the cache-key literal so unit tests can include this
// header and compare their mirror against the live literal. Pre-S4c,
// kPostProcessCacheKey was a `.cpp` static (not addressable from
// outside), so tests would fall back to string self-comparison
// ("mine == mine") and the drift detection would be a no-op (false
// green). The extern declaration gives every test a single source
// of truth; drift = test fails immediately.
//
// Naming: `kPostProcessCacheKeyCStr` (CStr suffix = "raw C-string"
// per the AY naming rules). The actual string literal lives in
// PostProcessPass.cpp as the canonical definition.
extern const char* const kPostProcessCacheKeyCStr;
extern const char* const kPostProcessFallbackCacheKeyCStr;

// Exact runtime shader sources used by PostProcessPass. Exposed from the
// private detail header so regression tests compile the production strings
// instead of stale test-local mirrors.
const char* postProcessPhoskiaSourceForTests() noexcept;
const char* postProcessFallbackPhoskiaSourceForTests() noexcept;

} // namespace ayt::render::detail
