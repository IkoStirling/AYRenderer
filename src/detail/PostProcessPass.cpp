#include "detail/PostProcessPass.h"

#include "detail/BloomBlurPass.h"  // Legacy direct-context fallback.
#include "detail/BloomPipeline.h"
#include "detail/FgResource.h"
#include "detail/GpuResources.h"
#include "detail/PostProcessPipeline.h"
#include "detail/RenderPass.h"     // §P5 (2026-08-24) — rateLimitedEarlyReturn helper
#include "detail/RenderResourceBlackboard.h"
#include "detail/SceneColorPipeline.h"

#include "AYShader/ShaderResource.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace ayt::render::detail
{

namespace {

// Post-process: sample sceneColor and the FrameGraph BloomSource, apply
// exposure to both scene-linear inputs, then tonemap (None / Reinhard /
// ACES) and display gamma. Production Bloom is accepted only when the current
// frame's BloomBlurB blackboard entry is valid. Direct contexts retain the
// BloomBlur latch fallback; otherwise execute() binds a safe texture and
// uploads zero strength.
// Knobs are vec4 (.x) for bgfx Vec4 upload ABI — see
// docs/pass-lessons-from-shadow.md §3.1. tonemapMode.x: 0=None,
// 1=Reinhard, 2=ACES (Narkowicz fitted). Select via
// mix(mix(none, reinhard, step(0.5,m)), aces, step(1.5,m)).
//
// UV.y flip in fragment (1 - vUv.y): Phoskia vertex blocks reject
// `let` before `out`. Same D3D RT vs backbuffer convention as shadows.
constexpr const char* kPostProcessPhoskiaSource = R"(
material PostProcess {
    texture2d sceneColor
    texture2d bloomTexture
    uniform vec4 bloomStrength
    uniform vec4 exposure
    uniform vec4 tonemapMode
    uniform vec4 gammaParams
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        let sampled = sample(sceneColor, uv)
        let bloomSample = sample(bloomTexture, uv)
        let raw = sampled.xyz * exposure.x
        let withBloom = raw
                      + bloomSample.xyz * bloomStrength.x * exposure.x
        let cx = max(withBloom.x, 0.0)
        let cy = max(withBloom.y, 0.0)
        let cz = max(withBloom.z, 0.0)
        let rx = cx / (1.0 + cx)
        let ry = cy / (1.0 + cy)
        let rz = cz / (1.0 + cz)
        let ax = (cx * (2.51 * cx + 0.03)) / (cx * (2.43 * cx + 0.59) + 0.14)
        let ay = (cy * (2.51 * cy + 0.03)) / (cy * (2.43 * cy + 0.59) + 0.14)
        let az = (cz * (2.51 * cz + 0.03)) / (cz * (2.43 * cz + 0.59) + 0.14)
        let m = tonemapMode.x
        let selX = mix(mix(cx, rx, step(0.5, m)), ax, step(1.5, m))
        let selY = mix(mix(cy, ry, step(0.5, m)), ay, step(1.5, m))
        let selZ = mix(mix(cz, rz, step(0.5, m)), az, step(1.5, m))
        let mx = max(selX, 0.0)
        let my = max(selY, 0.0)
        let mz = max(selZ, 0.0)
        let invG = 1.0 / max(gammaParams.x, 0.0001)
        let encoded = vec3(pow(mx, invG), pow(my, invG), pow(mz, invG))
        return vec4(encoded, sampled.w)
    }
}
)";

// v12 removes the dead uTime ABI and consumes sanitized display parameters.
constexpr const char* kPostProcessCacheKey =
    "postprocess_tonemap_aces_v12_sanitized_params_fs";

// Deliberately independent minimal fallback. If a primary-only language or
// math feature fails to compile, this still has a high chance of restoring a
// visible scene-color blit to the backbuffer.
constexpr const char* kPostProcessPassthroughSource = R"(
material PostProcessBlit {
    texture2d sceneColor
    vertex {
        in  pos : position
        out vUv : texcoord = pos.xy * vec2(0.5, 0.5) + vec2(0.5, 0.5)
        return vec4(pos.x, pos.y, 0.0, 1.0)
    }
    fragment {
        in  vUv : texcoord
        let uv = vec2(vUv.x, 1.0 - vUv.y)
        return sample(sceneColor, uv)
    }
}
)";
constexpr const char* kPostProcessPassthroughCacheKey =
    "postprocess_fallback_blit_v11_minimal_fs";

} // namespace

// §S4c (2026-07-23) — Bug fix #3 mirror (see DepthHazePass.cpp:118
// / BloomExtractPass.h / BloomBlurPass.h:185-199 for the originating
// pattern). Externalize the cache-key literal so unit tests can
// include PostProcessPass.h and compare their mirror against the
// live literal. Pre-S4c, kPostProcessCacheKey was a `.cpp` static
// (not addressable from outside), so tests would fall back to
// string self-comparison and drift detection would be a no-op
// (false green — same drift trap that bit Test_B5 in §P5.5 B).
// The extern declaration gives every test a single source of truth;
// drift = test fails immediately. S4c bumps the key to v4 (haze
// composite FS); future cuts bump to v5+.
//
// MUST live at file scope inside the `ayt::render::detail`
// namespace so the extern declaration in PostProcessPass.h finds it.
const char* const kPostProcessCacheKeyCStr = kPostProcessCacheKey;
const char* const kPostProcessFallbackCacheKeyCStr =
    kPostProcessPassthroughCacheKey;
const char* postProcessPhoskiaSourceForTests() noexcept
{
    return kPostProcessPhoskiaSource;
}

const char* postProcessFallbackPhoskiaSourceForTests() noexcept
{
    return kPostProcessPassthroughSource;
}

PostProcessPass::PostProcessPass() = default;
PostProcessPass::~PostProcessPass() = default;

uint32_t PostProcessPass::execute(PassExecContext& ctx)
{
    BGFXAdapter& adapter = ctx.adapter;
    shader::ShaderResourcePool& pool = ctx.pool;
    const FrameContext& frame = ctx.frame;
    // Own blit view — must not reuse the scene view id (would clobber
    // scene FBO binding + camera VP for every FO submit that frame).
    // After BloomExtract=10 / BlurH=11 / BlurV=12 / DepthHaze=13;
    // before UI=255. Forward + Deferred share kBlitViewId.
    const uint8_t viewId = kBlitViewId;
    const uint16_t viewportWidth  = ctx.viewportWidth;
    const uint16_t viewportHeight = ctx.viewportHeight;

    // Headless/uninitialized adapters must not allocate or submit resources.
    if (!adapter.isInitialized()) {
        // §P5 M3 (2026-08-24) — rate-limited log so a stuck
        // pipeline doesn't silently produce black frames for hours.
        rateLimitedEarlyReturn("PostProcessPass",
                               "adapter not initialized");
        return 0;
    }
    if (adapter.isNoopBackend()) {
        // R5+ — the Noop backend returns valid handles for everything
        // (so handle-validity checks can't distinguish "real backend
        // that's broken" from "Noop that should skip"). The shutdown
        // path under Noop is also fragile — bgfx::destroy on Noop
        // handles in a partial state has caused flakiness in the
        // Test_CaptureScreenshot path. Skip the pass entirely here:
        // it preserves the P0 contract (post-process is opt-in via
        // FrameContext knobs; default values = no-op image).
        // §P5 M3 (2026-08-24) — rate-limited log on the Noop path
        // (was silent; the host couldn't tell from a black viewport
        // whether the pipeline was actually drawing or stuck on the
        // Noop early-return).
        rateLimitedEarlyReturn("PostProcessPass",
                               "noop backend — post-process skipped");
        return 0;
    }

    if (viewportWidth == 0 || viewportHeight == 0) {
        // §P5 M3 (2026-08-24) — rate-limited log; zero-dim viewport
        // is a legitimate state (panel collapsed) but a sustained
        // zero during a normal frame is a pipeline regression.
        rateLimitedEarlyReturn("PostProcessPass",
                               "zero viewport");
        return 0;
    }

    const bgfx::FrameBufferHandle sourceFbo =
        selectSceneColorSourceFbo(ctx);
    if (!BGFXAdapter::isValid(sourceFbo)) {
        // §P5 M3 (2026-08-24) — rate-limited log on the
        // selectSourceFbo invalid path. Both deferred and forward
        // paths can produce this when their producers aren't
        // mounted yet (startup race); a sustained invalid is a
        // render-graph regression.
        rateLimitedEarlyReturn("PostProcessPass",
                               "sourceFbo invalid");
        return 0;
    }

    if (!_fullscreen.ensure(adapter)) {
        // §P5 M3 (2026-08-24) — rate-limited log; fullscreen-quad
        // VB/IB create-fail is rare but should leave a trail.
        rateLimitedEarlyReturn("PostProcessPass",
                               "fullscreen quad VB/IB invalid");
        return 0;
    }
    ensureProgram(pool);
    const bool primaryProgramReady = _program.isValid()
        && _programVariant == ProgramVariant::Primary
        && _uBloomStrength != ayt::shader::InvalidBinding
        && _uExposure      != ayt::shader::InvalidBinding
        && _uTonemapMode   != ayt::shader::InvalidBinding
        && _uGammaParams   != ayt::shader::InvalidBinding
        && _tSceneColor    != ayt::shader::InvalidBinding
        && _tBloomTexture  != ayt::shader::InvalidBinding;
    const bool fallbackProgramReady = _program.isValid()
        && _programVariant == ProgramVariant::Fallback
        && _tSceneColor != ayt::shader::InvalidBinding;
    const bool programReady = primaryProgramReady || fallbackProgramReady;

    const bgfx::TextureHandle fboColor = adapter.getFboAttachment(sourceFbo, 0);
    if (!BGFXAdapter::isValid(fboColor)) {
        // §P5 M3 (2026-08-24) — rate-limited log; missing color
        // attachment on a mounted FBO is a producer-side bug.
        rateLimitedEarlyReturn("PostProcessPass",
                               "sourceFbo color attachment invalid");
        return 0;
    }

    if (ctx.frameGraph == nullptr) {
        rateLimitedEarlyReturn("PostProcessPass", "frameGraph missing");
        return 0;
    }
    const bgfx::FrameBufferHandle finalLdr =
        ctx.frameGraph->resolve(FgResourceId::FinalLdrColor);
    if (!BGFXAdapter::isValid(finalLdr)) {
        rateLimitedEarlyReturn("PostProcessPass", "FinalLdrColor invalid");
        return 0;
    }

    // Single composite: sample HDR source color → display-referred RGBA8.
    // Do NOT bind sourceFbo as the draw target while sampling it
    // (same-FBO feedback clears/blacks the Editor viewport).
    //
    // The target is viewport-local; PresentPass applies the editor panel
    // offset when it copies this texture to the default backbuffer.
    configureFullscreenPassView(adapter, viewId, finalLdr,
                                0, 0, viewportWidth, viewportHeight);

    if (!programReady) {
        // §P5 M1 (2026-08-24) — kept the always-loud first-time
        // FATAL log (the very first occurrence is the signal that
        // something is wrong; it must not be suppressed), but
        // added a rate-limited periodic log so a sustained missing-
        // program state during long headless captures isn't silent.
        // rateLimitedEarlyReturn fires on frame==0 then every 256
        // frames — the helper handles suppression.
        static bool s_loggedMissing = false;
        if (!s_loggedMissing) {
            std::fprintf(stderr,
                "[PostProcessPass] FATAL: no composite program — "
                "FinalLdrColor was not produced. "
                "Check Phoskia acquire errors above.\n");
            s_loggedMissing = true;
        }
        rateLimitedEarlyReturn("PostProcessPass",
                               "no blit program — Phoskia acquire failed");
        return 0;
    }

    const ayt::shader::TextureHandle texHandle =
        ayt::render::detail::toShaderTexture(fboColor);
    // BloomSource is relevant only to the primary shader. The independent
    // fallback binds sceneColor alone and therefore cannot inherit a broken
    // primary binding ABI.
    ayt::shader::TextureHandle bloomTexHandle = texHandle;  // fallback
    bgfx::FrameBufferHandle bloomSourceFbo = BGFX_INVALID_HANDLE;
    bool bloomSourceReady = false;
    if (primaryProgramReady) {
        const BlackboardResourceEntry* bloom =
            ctx.resourceBlackboard != nullptr
                ? ctx.resourceBlackboard->findProduced(
                      BlackboardResourceId::BloomBlurB)
                : nullptr;
        const bool blurProduced = ctx.resourceBlackboard != nullptr
            ? bloom != nullptr
            : ctx.bloomBlurPass != nullptr
                && ctx.bloomBlurPass->producedThisFrame();
        if (bloom != nullptr) {
            bloomSourceFbo = bloom->framebuffer;
            if (BGFXAdapter::isValid(bloom->texture)) {
                bloomTexHandle =
                    ayt::render::detail::toShaderTexture(bloom->texture);
                bloomSourceReady = true;
            }
        } else if (blurProduced && ctx.frameGraph != nullptr) {
            bloomSourceFbo = ctx.frameGraph->resolveSemantic(
                ayt::render::detail::FgSemantic::BloomSource);
        }
        if (!bloomSourceReady && BGFXAdapter::isValid(bloomSourceFbo)) {
            const bgfx::TextureHandle bloomColor =
                adapter.getFboAttachment(bloomSourceFbo, 0);
            if (BGFXAdapter::isValid(bloomColor)) {
                bloomTexHandle =
                    ayt::render::detail::toShaderTexture(bloomColor);
                bloomSourceReady = true;
            }
        }
    }
    // bgfx Vec4 slots — pad scalars into .x (lessons §3.1).
    const float bloomPad[4] = {
        effectiveBloomStrength(frame.bloomStrength, bloomSourceReady),
        0.0f, 0.0f, 0.0f
    };
    const float exposurePad[4] = {
        sanitizePostProcessExposure(frame.exposure), 0.0f, 0.0f, 0.0f
    };
    const float tonemapPad[4] = {
        static_cast<float>(std::clamp(
            static_cast<int32_t>(frame.tonemapMode), int32_t{0}, int32_t{2})),
        0.0f, 0.0f, 0.0f
    };
    const float gammaPad[4] = {
        sanitizePostProcessGamma(frame.gamma), 0.0f, 0.0f, 0.0f
    };
    _fullscreen.bind(adapter);
    // Bind by recorded SAMPLER2D slots (pass stage=0 so setTexture does
    // not override compile-time units — see AYShader/ShaderResource.h).
    _program.setTexture(0, _tSceneColor, texHandle);
    if (primaryProgramReady) {
        _program.setTexture(0, _tBloomTexture, bloomTexHandle);
        _program.setUniform(_uBloomStrength, bloomPad, sizeof(bloomPad));
        _program.setUniform(_uExposure, exposurePad, sizeof(exposurePad));
        _program.setUniform(_uTonemapMode, tonemapPad, sizeof(tonemapPad));
        _program.setUniform(_uGammaParams, gammaPad, sizeof(gammaPad));
    }

    ayt::shader::DrawCallContext sub;
    sub.viewId = viewId;
    sub.state  = 0;  // P6.5: per-draw state owned by Adapter (see
                       // setStateDepthTestAlways() called below).
    // P6.5 (2026-07-22) — preset state replaces the inline
    // `BGFX_STATE_WRITE_RGB | WRITE_A | DEPTH_TEST_ALWAYS`. Bit
    // combination identical.
    adapter.setStateDepthTestAlways();
    _program.submit(sub);
    ctx.frameGraph->markProduced(FgResourceId::FinalLdrColor);

    static bool s_loggedSubmit = false;
    if (!s_loggedSubmit) {
        const bool bloomFromPong =
            BGFXAdapter::isValid(bloomSourceFbo)
            && (bloomTexHandle.id != texHandle.id);
        std::fprintf(stderr,
            "[PostProcessPass] FinalLdrColor ok view=%u size=(%u,%u) "
            "gamma=%.1f exposure=%.2f tonemap=%.0f bloom=%.2f "
            "bloomSrc=%s variant=%s\n",
            static_cast<unsigned>(viewId),
            static_cast<unsigned>(viewportWidth),
            static_cast<unsigned>(viewportHeight),
            gammaPad[0],
            exposurePad[0],
            tonemapPad[0],
            bloomPad[0],
            bloomFromPong ? "pong" : "fallback(scene)",
            primaryProgramReady ? "primary" : "fallback");
        s_loggedSubmit = true;
    }
    // Successful submission is steady-state, not an early return.  Keep the
    // detailed one-shot message above and leave periodic diagnostics for
    // actual failure paths.
    return 1;
}

void PostProcessPass::ensureProgram(shader::ShaderResourcePool& pool)
{
    if (_program.isValid() && _programVariant == ProgramVariant::Primary) {
        return;
    }

    // A working fallback stays active while the primary compile is retried at
    // a bounded cadence. With no program, the same delay prevents a missing
    // shaderc/toolchain from stalling every frame.
    if (_programRetryFrames > 0) {
        --_programRetryFrames;
        return;
    }

    ayt::shader::ShaderResource primary =
        pool.acquire(kPostProcessPhoskiaSource, kPostProcessCacheKey);
    if (primary.isValid()) {
        const ayt::shader::BindingId bloomStrength =
            primary.getUniformBinding("bloomStrength");
        const ayt::shader::BindingId exposure =
            primary.getUniformBinding("exposure");
        const ayt::shader::BindingId tonemapMode =
            primary.getUniformBinding("tonemapMode");
        const ayt::shader::BindingId gammaParams =
            primary.getUniformBinding("gammaParams");
        const ayt::shader::BindingId sceneColor =
            primary.getTextureBinding("sceneColor");
        const ayt::shader::BindingId bloomTexture =
            primary.getTextureBinding("bloomTexture");
        const bool bindingsReady =
            bloomStrength != ayt::shader::InvalidBinding
            && exposure != ayt::shader::InvalidBinding
            && tonemapMode != ayt::shader::InvalidBinding
            && gammaParams != ayt::shader::InvalidBinding
            && sceneColor != ayt::shader::InvalidBinding
            && bloomTexture != ayt::shader::InvalidBinding;
        if (bindingsReady) {
            _program = std::move(primary);
            _uBloomStrength = bloomStrength;
            _uExposure = exposure;
            _uTonemapMode = tonemapMode;
            _uGammaParams = gammaParams;
            _tSceneColor = sceneColor;
            _tBloomTexture = bloomTexture;
            _programVariant = ProgramVariant::Primary;
            _programRetryFrames = 0;
            return;
        }
        std::fprintf(stderr,
                     "[PostProcessPass] primary program bindings incomplete; "
                     "keeping/trying minimal fallback\n");
    } else {
        std::fprintf(stderr,
                     "[PostProcessPass] tonemap Phoskia acquire failed; "
                     "keeping/trying minimal fallback\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[PostProcessPass]   %s\n", err.c_str());
        }
    }

    constexpr uint16_t kRetryIntervalFrames = 120;
    if (_program.isValid() && _programVariant == ProgramVariant::Fallback) {
        _programRetryFrames = kRetryIntervalFrames;
        return;
    }

    ayt::shader::ShaderResource fallback =
        pool.acquire(kPostProcessPassthroughSource,
                     kPostProcessPassthroughCacheKey);
    const ayt::shader::BindingId sceneColor = fallback.isValid()
        ? fallback.getTextureBinding("sceneColor")
        : ayt::shader::InvalidBinding;
    if (!fallback.isValid()
        || sceneColor == ayt::shader::InvalidBinding) {
        _program.reset();
        _programVariant = ProgramVariant::None;
        _programRetryFrames = kRetryIntervalFrames;
        std::fprintf(stderr,
                     "[PostProcessPass] minimal fallback acquire failed; "
                     "post-process will skip blit (scene may stay offscreen)\n");
        for (const std::string& err : pool.lastCompileErrors()) {
            std::fprintf(stderr, "[PostProcessPass]   %s\n", err.c_str());
        }
        return;
    }

    _program = std::move(fallback);
    _uBloomStrength = ayt::shader::InvalidBinding;
    _uExposure = ayt::shader::InvalidBinding;
    _uTonemapMode = ayt::shader::InvalidBinding;
    _uGammaParams = ayt::shader::InvalidBinding;
    _tSceneColor = sceneColor;
    _tBloomTexture = ayt::shader::InvalidBinding;
    _programVariant = ProgramVariant::Fallback;
    _programRetryFrames = kRetryIntervalFrames;
}

void PostProcessPass::destroyResources(BGFXAdapter& adapter)
{
    _fullscreen.destroy(adapter);
    if (_program.isValid()) {
        // R5.1 — release the ShaderResource. ShaderResource doesn't
        // carry a back-pointer to its pool; the pool reference is
        // held by Renderer::Impl and outlives this pass (see
        // AYRenderer.cpp:160 shutdown order: resources → shaderPool
        // → adapter). Resetting _program here is the documented
        // ShaderResource contract (see ShaderResource::reset in
        // AYShader/ShaderResource.h:32) — the pool's dtor releases the
        // underlying GPU program once the refcount hits zero,
        // regardless of which ShaderResource instance held the last
        // reference.
        _program.reset();
    }
    _uBloomStrength = ayt::shader::InvalidBinding;
    _uExposure      = ayt::shader::InvalidBinding;
    _uTonemapMode   = ayt::shader::InvalidBinding;
    _uGammaParams   = ayt::shader::InvalidBinding;
    _tSceneColor    = ayt::shader::InvalidBinding;
    _tBloomTexture  = ayt::shader::InvalidBinding;
    _programVariant = ProgramVariant::None;
    _programRetryFrames = 0;
}

bgfx::FrameBufferHandle PostProcessPass::selectSourceFbo(
    const PassExecContext& ctx) noexcept
{
    return selectSceneColorSourceFbo(ctx);
}

} // namespace ayt::render::detail
